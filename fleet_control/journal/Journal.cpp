#include "Journal.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <google/protobuf/util/time_util.h>
#include <spdlog/spdlog.h>

#include "missions/MissionStore.hpp"   // now_ms

namespace dbv1 = fms::database::v1;
using Control = dbv1::ControlStorage::Stub;
using google::protobuf::util::TimeUtil;

namespace {

constexpr size_t kBatch = 500;          // событий в одном AppendEvents
constexpr size_t kMaxPending = 20000;   // больше — база давно лежит, старые выбрасываем
constexpr auto kRetryDelay = std::chrono::seconds(2);

void log_failure(std::exception_ptr error) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        spdlog::error("[journal] coroutine failed: {}", e.what());
    }
}

}  // namespace

Journal::Journal(asio::io_context& io_context, Services& services)
    : io_context_(io_context), services_(services), wakeup_(io_context, 1), retry_timer_(io_context) {}

void Journal::start() {
    asio::co_spawn(io_context_, write_loop(), log_failure);
}

void Journal::stop() {
    stopped_ = true;
    wakeup_.close();
    retry_timer_.cancel();
}

void Journal::add(const std::string& source, const JournalEvent& event, const std::string& robot_id,
                  const std::string& mission_id) {
    if (pending_.size() >= kMaxPending) {
        pending_.pop_front();
        spdlog::warn("[journal] database is not keeping up, the oldest event is dropped");
    }
    dbv1::Event& record = pending_.emplace_back();
    *record.mutable_at() = TimeUtil::MillisecondsToTimestamp(now_ms());
    record.set_source(source);
    record.set_type(event.type);
    record.set_robot_id(robot_id);
    record.set_mission_id(mission_id);
    record.set_details_json(event.details.dump());
    wakeup_.try_send(boost::system::error_code{});
}

asio::awaitable<void> Journal::write_loop() {
    while (!stopped_) {
        if (pending_.empty()) {
            auto received = co_await wakeup_.async_receive(asio::as_tuple(asio::use_awaitable));
            auto& [ec] = received;
            if (ec) {
                break;   // stop()
            }
            continue;
        }

        // Пачка с начала очереди; пока пишем, новые встают в конец — порядок не меняется
        const size_t count = std::min(pending_.size(), kBatch);
        dbv1::AppendEventsRequest request;
        for (size_t i = 0; i < count; ++i) {
            *request.add_events() = pending_[i];
        }
        dbv1::AppendEventsResponse response;
        grpc::Status status =
            co_await services_.call<&Control::PrepareAsyncAppendEvents>(*services_.control, request, response);

        if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
            status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
            if (!stopped_) {   // пачка остаётся в начале очереди — повторим её же
                retry_timer_.expires_after(kRetryDelay);
                co_await retry_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
            }
            continue;
        }
        if (!status.ok()) {
            spdlog::error("[journal] {} events not saved: {}", count, status.error_message());   // ошибка в control
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(std::min(count, pending_.size())));
    }
}