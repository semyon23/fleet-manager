#include "MissionStore.hpp"
#include <algorithm>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>
#include <google/protobuf/util/time_util.h>
namespace dbv1 = fms::database::v1;
using Control = dbv1::ControlStorage::Stub;
using google::protobuf::util::TimeUtil;

namespace {

constexpr auto kRetryDelay = std::chrono::seconds(2);


dbv1::MissionRecord record_of(const Mission& mission) {
    dbv1::MissionRecord record;
    record.set_mission_id(mission.id);
    record.set_map_id(mission.map_id);
    record.set_robot_id(mission.robot_id);
    record.set_state(mission.state);
    record.set_priority(mission.priority);
    record.set_body_json(mission_json(mission).dump());
    *record.mutable_created_at() = TimeUtil::MillisecondsToTimestamp(mission.created_at_ms);
    *record.mutable_updated_at() = TimeUtil::MillisecondsToTimestamp(mission.updated_at_ms);
    if (mission.finished_at_ms) {
        *record.mutable_finished_at() = TimeUtil::MillisecondsToTimestamp(*mission.finished_at_ms);
    }
    return record;
}

void log_failure(std::exception_ptr error) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        spdlog::error("[mission_store] coroutine failed: {}", e.what());
    }
}

}  // namespace

MissionStore::MissionStore(asio::io_context& io_context, Services& services)
    : io_context_(io_context), services_(services), wakeup_(io_context, 1), retry_timer_(io_context) {}

void MissionStore::start() {
    asio::co_spawn(io_context_, load(), log_failure);
    asio::co_spawn(io_context_, write_loop(), log_failure);
}

void MissionStore::stop() {
    stopped_ = true;
    wakeup_.close();
    retry_timer_.cancel();
}

// ==================================================================== миссии

asio::awaitable<grpc::Status> MissionStore::create(Mission mission) {
    const std::string id = mission.id;
    dbv1::SaveMissionRequest request;
    *request.mutable_mission() = record_of(mission);

    // Сначала в память: повторный POST с тем же id, пока мы ждём базу, её увидит
    auto [it, inserted] = active_.emplace(id, std::move(mission));
    if (!inserted) {
        co_return grpc::Status{grpc::StatusCode::ALREADY_EXISTS, "mission " + id + " already exists"};
    }

    dbv1::SaveMissionResponse response;
    grpc::Status status = co_await services_.call<&Control::PrepareAsyncSaveMission>(*services_.control, request, response);
    if (!status.ok()) {
        active_.erase(id);
        spdlog::warn("[mission_store] mission {} not saved: {}", id, status.error_message());
        co_return status;
    }
    spdlog::info("[mission_store] mission {} queued", id);
    if (on_queue_change) on_queue_change();
    co_return status;
}

asio::awaitable<std::pair<grpc::Status, std::optional<Mission>>> MissionStore::get(const std::string& id) {
    if (auto it = active_.find(id); it != active_.end()) {
        co_return std::pair{grpc::Status::OK, std::optional<Mission>(it->second)};
    }
    // Только что завершилась и ещё не записана — в базе пока старая версия, новая лежит в очереди
    if (auto it = pending_.find(id); it != pending_.end()) {
        co_return std::pair{grpc::Status::OK, std::optional<Mission>(mission_from_json(nlohmann::json::parse(it->second.body_json())))};
    }

    dbv1::GetMissionRequest request;
    request.set_mission_id(id);
    dbv1::GetMissionResponse response;
    grpc::Status status = co_await services_.call<&Control::PrepareAsyncGetMission>(*services_.control, request, response);
    if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
        co_return std::pair{grpc::Status::OK, std::optional<Mission>()};
    }
    if (!status.ok()) {
        co_return std::pair{status, std::optional<Mission>()};
    }
    co_return std::pair{status, std::optional<Mission>(mission_from_json(nlohmann::json::parse(response.mission().body_json())))};
}

Mission* MissionStore::find(const std::string& id) {
    auto it = active_.find(id);
    return it == active_.end() ? nullptr : &it->second;
}

std::vector<Mission*> MissionStore::queue() {
    std::vector<Mission*> queued;
    for (auto& [id, mission] : active_) {
        if (mission.state == "QUEUED") {
            queued.push_back(&mission);
        }
    }
    std::ranges::sort(queued, [](const Mission* a, const Mission* b) {
        if (a->priority != b->priority) {
            return a->priority > b->priority;            // важнее — раньше
        }
        if (a->created_at_ms != b->created_at_ms) {
            return a->created_at_ms < b->created_at_ms;  // при равном приоритете — кто раньше пришёл
        }
        return a->id < b->id;                            // и чтобы порядок был всегда один и тот же
    });
    return queued;
}

nlohmann::json MissionStore::changed(Mission& mission) {
    mission.updated_at_ms = now_ms();
    nlohmann::json document = mission_json(mission);
    pending_[mission.id] = record_of(mission);   // за время записи поменялась ещё раз — уйдёт последняя версия
    wakeup_.try_send(boost::system::error_code{});
    if (mission.finished()) {
        const std::string id = mission.id;   // mission — ссылка в active_: после erase её нет
        active_.erase(id);
        if (on_queue_change) on_queue_change();
    }
    return document;
}

// ==================================================================== база

asio::awaitable<void> MissionStore::load() {
    bool warned = false;
    while (!stopped_) {
        dbv1::ListActiveMissionsRequest request;
        dbv1::ListActiveMissionsResponse response;
        grpc::Status status =
            co_await services_.call<&Control::PrepareAsyncListActiveMissions>(*services_.control, request, response);
        if (status.ok()) {
            for (const auto& record : response.missions()) {
                try {
                    Mission mission = mission_from_json(nlohmann::json::parse(record.body_json()));
                    active_.emplace(mission.id, std::move(mission));
                } catch (const std::exception& e) {
                    // Документ пишет только control — кривой значит ошибку в коде или ручную правку базы
                    spdlog::error("[mission_store] mission {} skipped: {}", record.mission_id(), e.what());
                }
            }
            loaded_ = true;
            spdlog::info("[mission_store] {} active missions loaded", active_.size());
            if (on_queue_change) on_queue_change();
            co_return;
        }
        if (!warned) {   // база ещё не поднялась — не спамить каждые 2 с
            spdlog::warn("[mission_store] cannot load missions: {}; retrying", status.error_message());
            warned = true;
        }
        retry_timer_.expires_after(kRetryDelay);
        co_await retry_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }
}

asio::awaitable<void> MissionStore::write_loop() {
    while (!stopped_) {
        if (pending_.empty()) {
            auto [ec] = co_await wakeup_.async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                break;   // stop()
            }
            continue;
        }

        auto batch = std::move(pending_);
        pending_.clear();
        bool retry = false;
        for (auto& [id, record] : batch) {
            dbv1::SaveMissionRequest request;
            *request.mutable_mission() = record;
            dbv1::SaveMissionResponse response;
            grpc::Status status =
                co_await services_.call<&Control::PrepareAsyncSaveMission>(*services_.control, request, response);
            if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
                status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
                pending_.try_emplace(id, std::move(record));   // за это время могла прийти новее — она важнее
                retry = true;
            } else if (!status.ok()) {
                spdlog::error("[mission_store] mission {} not saved: {}", id, status.error_message());
            }
        }

        if (retry && !stopped_) {
            retry_timer_.expires_after(kRetryDelay);
            co_await retry_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        }
    }
}


std::set<std::string> MissionStore::busy_robots() const {
    std::set<std::string> robots;
    for (const auto& [id, mission] : active_) {
        if (!mission.robot_id.empty()) {
            robots.insert(mission.robot_id);
        }
    }
    return robots;
}

std::vector<Mission*> MissionStore::active() {
    std::vector<Mission*> missions;
    for (auto& [id, mission] : active_) {
        missions.push_back(&mission);
    }
    return missions;
}