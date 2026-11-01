#include "../include/FleetSync.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

namespace dbv1 = fms::database::v1;
namespace dpv1 = fms::dispatch::v1;
using Registry = dbv1::RobotRegistry::Stub;
using Dispatch = dpv1::FleetDispatch::Stub;

constexpr auto kSyncInterval = std::chrono::seconds(10);
constexpr auto kReconnectDelay = std::chrono::seconds(2);

FleetSync::FleetSync(asio::io_context& io_context, Services& services)
    : io_context_(io_context), services_(services), reconnect_timer_(io_context), sync_timer_(io_context) {}

void FleetSync::start() {
    auto log_if_failed = [](std::exception_ptr error) {
        if (!error) return;
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            spdlog::error("[fleet_sync] coroutine failed: {}", e.what());
        }
    };
    asio::co_spawn(io_context_, watch_dispatch(), log_if_failed);
    asio::co_spawn(io_context_, sync_loop(), log_if_failed);
}

void FleetSync::stop() {
    stopped_ = true;
    if (stream_) {
        stream_->context().TryCancel();   // read() вернёт false, корутина выйдет
    }
    reconnect_timer_.cancel();
    sync_timer_.cancel();
}

// ==================================================================== стрим статусов dispatch

asio::awaitable<void> FleetSync::watch_dispatch() {
    while (!stopped_) {
        StatusStreamRPC rpc{services_.grpc_context};
        stream_ = &rpc;
        dpv1::StreamRobotStatusRequest request;   // все роботы
        dpv1::RobotStatusUpdate update;
        if (co_await rpc.start(*services_.dispatch, request, asio::use_awaitable)) {
            while (co_await rpc.read(update, asio::use_awaitable)) {
                on_update(update);
            }
        }
        grpc::Status status = co_await rpc.finish(asio::use_awaitable);
        stream_ = nullptr;

        if (connected_) {
            spdlog::warn("[fleet_sync] dispatch stream closed: {}", status.error_message());
        }
        connected_ = false;
        in_dispatch_.clear();
        if (stopped_) {
            break;
        }
        reconnect_timer_.expires_after(kReconnectDelay);
        co_await reconnect_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }
}

void FleetSync::on_update(const dpv1::RobotStatusUpdate& update) {
    if (update.has_snapshot()) {
        in_dispatch_.clear();
        for (const auto& robot : update.snapshot().robots()) {
            in_dispatch_.insert(robot.robot_id());
        }
        connected_ = true;
        spdlog::info("[fleet_sync] connected to dispatch, it has {} robots", in_dispatch_.size());
        sync_timer_.cancel();   // сверить сейчас, не дожидаясь 10 с
    } else if (update.has_status()) {
        in_dispatch_.insert(update.status().robot_id());
    } else if (update.has_removed_robot_id()) {
        in_dispatch_.erase(update.removed_robot_id());
    }
}

// ==================================================================== сверка с базой

asio::awaitable<void> FleetSync::sync_loop() {
    while (!stopped_) {
        if (connected_) {
            co_await sync();
        }
        sync_timer_.expires_after(kSyncInterval);
        co_await sync_timer_.async_wait(asio::as_tuple(asio::use_awaitable));   // отменён: подключились или stop()
    }
}

asio::awaitable<void> FleetSync::sync() {
    dbv1::ListRobotsRequest request;
    request.set_include_deleting(true);
    dbv1::ListRobotsResponse response;
    grpc::Status status = co_await services_.call<&Registry::PrepareAsyncListRobots>(*services_.registry, request, response);
    if (!status.ok()) {
        if (database_ok_) {
            spdlog::warn("[fleet_sync] database unavailable ({}), robots are not reconciled", status.error_message());
            database_ok_ = false;
        }
        co_return;
    }
    if (!database_ok_) {
        spdlog::info("[fleet_sync] database is available again");
        database_ok_ = true;
    }

    for (const auto& record : response.robots()) {
        const auto& spec = record.spec();
        if (spec.lifecycle() == dbv1::ROBOT_LIFECYCLE_PENDING_DELETE) {
            spdlog::info("[fleet_sync] finishing delete of robot {}", spec.robot_id());
            co_await finish_delete(spec.robot_id());
        } else if (!in_dispatch_.contains(spec.robot_id())) {
            spdlog::info("[fleet_sync] robot {} is not in dispatch, adding it", spec.robot_id());
            co_await add_to_dispatch(spec);
        }
    }
}

// ==================================================================== общие шаги

// Робот уже в базе. Не вышло — не беда: сверка повторит, когда dispatch будет доступен
asio::awaitable<void> FleetSync::add_to_dispatch(const dbv1::RobotSpec& spec) {
    dpv1::AddRobotRequest request;
    request.set_robot_id(spec.robot_id());
    request.set_manufacturer(spec.manufacturer());
    request.set_serial_number(spec.serial_number());
    dpv1::AddRobotResponse response;
    grpc::Status status = co_await services_.call<&Dispatch::PrepareAsyncAddRobot>(*services_.dispatch, request, response);
    if (!status.ok()) {
        spdlog::warn("[fleet_sync] robot {} is not added to dispatch: {}", spec.robot_id(), status.error_message());
    }
}

// dispatch завершает миссию (ROBOT_REMOVED) и отписывается от робота, потом запись удаляется из базы.
// Не вышло — робот остаётся PENDING_DELETE: повторный DELETE или следующая сверка доведут удаление.
asio::awaitable<void> FleetSync::finish_delete(const std::string& robot_id) {
    dpv1::RemoveRobotRequest remove;
    remove.set_robot_id(robot_id);
    dpv1::RemoveRobotResponse removed;
    grpc::Status status = co_await services_.call<&Dispatch::PrepareAsyncRemoveRobot>(*services_.dispatch, remove, removed);
    if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {   // NOT_FOUND: dispatch его не знал
        spdlog::warn("[fleet_sync] robot {} stays PENDING_DELETE, dispatch did not remove it: {}", robot_id,
                     status.error_message());
        co_return;
    }

    dbv1::DeleteRobotRequest erase;
    erase.set_robot_id(robot_id);
    dbv1::DeleteRobotResponse erased;
    status = co_await services_.call<&Registry::PrepareAsyncDeleteRobot>(*services_.registry, erase, erased);
    if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {   // NOT_FOUND: уже удалён
        spdlog::warn("[fleet_sync] robot {} stays PENDING_DELETE, database did not delete it: {}", robot_id,
                     status.error_message());
        co_return;
    }
    spdlog::info("[fleet_sync] robot {} deleted", robot_id);
}