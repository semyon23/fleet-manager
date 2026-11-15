#include "AlarmEngine.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <google/protobuf/util/time_util.h>
#include <nlohmann/json.hpp>
#include <random>
#include <spdlog/spdlog.h>

#include "missions/MissionStore.hpp"   // now_ms

namespace dbv1 = fms::database::v1;
using Control = dbv1::ControlStorage::Stub;
using google::protobuf::util::TimeUtil;

namespace {

constexpr auto kTick = std::chrono::seconds(1);
constexpr auto kRetryDelay = std::chrono::seconds(2);
const std::string kServiceDownKey = "SERVICE_DOWN/fleet_dispatch";

void log_failure(std::exception_ptr error) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        spdlog::error("[alarms] coroutine failed: {}", e.what());
    }
}

std::string new_alarm_id() {
    static std::mt19937 random{std::random_device{}()};
    static constexpr char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<size_t> pick(0, sizeof(kChars) - 2);
    std::string id = "al-";
    for (int i = 0; i < 8; ++i) id += kChars[pick(random)];
    return id;
}

}  // namespace

AlarmEngine::AlarmEngine(asio::io_context& io_context, Services& services, double low_battery_percent)
    : io_context_(io_context), services_(services), low_battery_percent_(low_battery_percent),
      wakeup_(io_context, 1), tick_timer_(io_context), retry_timer_(io_context) {}

void AlarmEngine::start() {
    asio::co_spawn(io_context_, load(), log_failure);
    asio::co_spawn(io_context_, tick_loop(), log_failure);
    asio::co_spawn(io_context_, write_loop(), log_failure);
}

void AlarmEngine::stop() {
    stopped_ = true;
    wakeup_.close();
    tick_timer_.cancel();
    retry_timer_.cancel();
}

// ==================================================================== входы

void AlarmEngine::on_robot(const std::string& robot_id, const fms::v1::RobotStatus* status) {
    robots_[robot_id] = status ? std::optional(*status) : std::nullopt;
    evaluate(robot_id);
}

void AlarmEngine::remove_robot(const std::string& robot_id) {
    robots_.erase(robot_id);
    std::erase_if(waiting_, [&](const auto& item) { return item.second.robot_id == robot_id; });
    std::vector<std::string> ids;
    for (const auto& [id, alarm] : open_) {
        if (alarm.robot_id() == robot_id) ids.push_back(id);
    }
    for (const auto& id : ids) {
        auto it = open_.find(id);
        if (it->second.active()) clear(id, "robot removed from fms");
        it = open_.find(id);
        if (it != open_.end()) acknowledge(it->second, "fms");   // действовать с ней больше некому
    }
}

void AlarmEngine::keep_only(const std::set<std::string>& robot_ids) {
    known_robots_ = robot_ids;
    if (!loaded_) {
        return;   // применим после загрузки: открытых тревог ещё нет в памяти
    }
    std::set<std::string> gone;
    for (const auto& [id, alarm] : open_) {
        if (!alarm.robot_id().empty() && !robot_ids.contains(alarm.robot_id())) gone.insert(alarm.robot_id());
    }
    for (const auto& [robot_id, status] : robots_) {
        if (!robot_ids.contains(robot_id)) gone.insert(robot_id);
    }
    for (const auto& robot_id : gone) remove_robot(robot_id);
}

void AlarmEngine::set_dispatch_connected(bool connected) {
    dispatch_connected_ = connected;
    if (!loaded_) return;
    if (!connected) {
        want({kServiceDownKey, "SERVICE_DOWN", "CRITICAL", "Service down",
              "fms has no connection to fleet_dispatch: robots are not controlled", 5000}, "");
        return;
    }
    waiting_.erase(kServiceDownKey);
    if (auto it = active_by_key_.find(kServiceDownKey); it != active_by_key_.end()) {
        clear(it->second, "connection restored");
    }
    // Статусы роботов придут снимком из dispatch — on_robot пересчитает
}

std::optional<dbv1::AlarmRecord> AlarmEngine::ack(const std::string& alarm_id, const std::string& by) {
    auto it = open_.find(alarm_id);
    if (it == open_.end()) {
        return std::nullopt;
    }
    dbv1::AlarmRecord alarm = it->second;
    if (!alarm.has_acknowledged_at()) {
        acknowledge(it->second, by);
        alarm = pending_[alarm_id].record;   // как будет записана
    }
    return alarm;   // уже подтверждена (активная) — как есть
}

// ==================================================================== правила → тревоги

void AlarmEngine::evaluate(const std::string& robot_id) {
    if (!loaded_ || !dispatch_connected_) {
        return;   // до загрузки — не создать дубль; без dispatch статусы устарели (подавление)
    }
    const auto& status = robots_[robot_id];
    const auto conditions = robot_alarm_conditions(robot_id, status ? &*status : nullptr, low_battery_percent_);

    std::set<std::string> keys;
    for (const auto& condition : conditions) {
        keys.insert(condition.key);
        want(condition, robot_id);
    }
    // Условия ушли: ждущие — забыть, активные — снять
    std::erase_if(waiting_, [&](const auto& item) {
        return item.second.robot_id == robot_id && !keys.contains(item.first);
    });
    std::vector<std::string> gone;
    for (const auto& [key, id] : active_by_key_) {
        if (open_.at(id).robot_id() == robot_id && !keys.contains(key)) gone.push_back(id);
    }
    for (const auto& id : gone) clear(id, "");
}

void AlarmEngine::want(const AlarmCondition& condition, const std::string& robot_id) {
    if (active_by_key_.contains(condition.key)) {
        return;   // уже горит
    }
    if (condition.delay_ms == 0) {
        raise(condition, robot_id);
        return;
    }
    waiting_.try_emplace(condition.key, Waiting{condition, robot_id, now_ms()});   // с первого раза, не с последнего
}

void AlarmEngine::raise(const AlarmCondition& condition, const std::string& robot_id) {
    waiting_.erase(condition.key);
    dbv1::AlarmRecord alarm;
    alarm.set_alarm_id(new_alarm_id());
    alarm.set_code(condition.code);
    alarm.set_severity(condition.severity);
    alarm.set_source(robot_id.empty() ? "SYSTEM" : "ROBOT");
    alarm.set_robot_id(robot_id);
    alarm.set_title(condition.title);
    alarm.set_detail(condition.detail);
    alarm.set_key(condition.key);
    alarm.set_active(true);
    *alarm.mutable_created_at() = TimeUtil::MillisecondsToTimestamp(now_ms());
    spdlog::warn("[alarms] {} {}: {}", alarm.alarm_id(), condition.code, condition.detail);
    open_[alarm.alarm_id()] = alarm;
    active_by_key_[condition.key] = alarm.alarm_id();
    save(alarm, "RAISED");
}

void AlarmEngine::clear(const std::string& alarm_id, const std::string& why) {
    auto it = open_.find(alarm_id);
    if (it == open_.end() || !it->second.active()) return;
    dbv1::AlarmRecord& alarm = it->second;
    alarm.set_active(false);
    *alarm.mutable_cleared_at() = TimeUtil::MillisecondsToTimestamp(now_ms());
    active_by_key_.erase(alarm.key());
    spdlog::info("[alarms] {} {} cleared", alarm_id, alarm.code());
    const dbv1::AlarmRecord copy = alarm;
    if (alarm.has_acknowledged_at()) open_.erase(it);   // снята и подтверждена — закрыта
    save(copy, "CLEARED", why);
}

void AlarmEngine::acknowledge(dbv1::AlarmRecord& alarm, const std::string& by) {
    *alarm.mutable_acknowledged_at() = TimeUtil::MillisecondsToTimestamp(now_ms());
    alarm.set_acknowledged_by(by);
    const dbv1::AlarmRecord copy = alarm;
    if (!alarm.active()) open_.erase(alarm.alarm_id());   // ссылка alarm после этого недействительна
    save(copy, "ACKED");
}

void AlarmEngine::save(const dbv1::AlarmRecord& alarm, const std::string& event_type, const std::string& why) {
    nlohmann::json details{{"code", alarm.code()}, {"severity", alarm.severity()}, {"title", alarm.title()}};
    if (!why.empty()) details["reason"] = why;
    if (event_type == "ACKED" && !alarm.acknowledged_by().empty()) details["by"] = alarm.acknowledged_by();

    Pending& pending = pending_[alarm.alarm_id()];
    pending.record = alarm;   // последняя версия
    dbv1::Event& event = pending.events.emplace_back();   // а события копятся все
    *event.mutable_at() = TimeUtil::MillisecondsToTimestamp(now_ms());
    event.set_source("ALARM");
    event.set_type(event_type);
    event.set_robot_id(alarm.robot_id());
    event.set_alarm_id(alarm.alarm_id());
    event.set_details_json(details.dump());
    wakeup_.try_send(boost::system::error_code{});
}

// ==================================================================== база и таймер

asio::awaitable<void> AlarmEngine::load() {
    bool warned = false;
    while (!stopped_) {
        dbv1::ListOpenAlarmsRequest request;
        dbv1::ListOpenAlarmsResponse response;
        grpc::Status status =
            co_await services_.call<&Control::PrepareAsyncListOpenAlarms>(*services_.control, request, response);
        if (status.ok()) {
            for (const auto& alarm : response.alarms()) {
                open_[alarm.alarm_id()] = alarm;
                if (alarm.active()) active_by_key_[alarm.key()] = alarm.alarm_id();
            }
            loaded_ = true;
            spdlog::info("[alarms] {} open alarms loaded", open_.size());
            if (known_robots_) keep_only(*known_robots_);   // тревоги роботов, которых в fms уже нет

            // Пока грузили, статусы уже шли — теперь их можно сверить с тревогами
            set_dispatch_connected(dispatch_connected_);
            for (const auto& [robot_id, robot_status] : robots_) evaluate(robot_id);
            co_return;
        }
        if (!warned) {
            spdlog::warn("[alarms] cannot load alarms: {}; retrying", status.error_message());
            warned = true;
        }
        retry_timer_.expires_after(kRetryDelay);
        co_await retry_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }
}

asio::awaitable<void> AlarmEngine::tick_loop() {
    while (!stopped_) {
        tick_timer_.expires_after(kTick);
        co_await tick_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        const int64_t now = now_ms();
        std::vector<Waiting> due;
        for (const auto& [key, waiting] : waiting_) {
            if (now - waiting.since_ms >= waiting.condition.delay_ms) due.push_back(waiting);
        }
        for (const auto& waiting : due) raise(waiting.condition, waiting.robot_id);
    }
}

asio::awaitable<void> AlarmEngine::write_loop() {
    while (!stopped_) {
        if (pending_.empty()) {
            auto received = co_await wakeup_.async_receive(asio::as_tuple(asio::use_awaitable));
            auto& [ec] = received;
            if (ec) break;   // stop()
            continue;
        }

        auto batch = std::move(pending_);
        pending_.clear();
        bool retry = false;
        for (auto& [id, item] : batch) {
            dbv1::SaveAlarmRequest request;
            *request.mutable_alarm() = item.record;
            for (const auto& event : item.events) *request.add_events() = event;
            dbv1::SaveAlarmResponse response;
            grpc::Status status =
                co_await services_.call<&Control::PrepareAsyncSaveAlarm>(*services_.control, request, response);
            if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
                status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
                // За это время могла прийти новее: тревога — её, а наши события случились раньше её событий
                auto [it, inserted] = pending_.try_emplace(id, std::move(item));
                if (!inserted) {
                    it->second.events.insert(it->second.events.begin(), item.events.begin(), item.events.end());
                }
                retry = true;
            } else if (!status.ok()) {
                spdlog::error("[alarms] alarm {} not saved: {}", id, status.error_message());
            }
        }
        if (retry && !stopped_) {
            retry_timer_.expires_after(kRetryDelay);
            co_await retry_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        }
    }
}