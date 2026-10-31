#include "../include/DatabaseWriter.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "spdlog/spdlog.h"

namespace dbv1 = fms::database::v1;

using SaveRobotStatusRPC = agrpc::ClientRPC<&dbv1::DispatchStorage::Stub::PrepareAsyncSaveRobotStatus>;
using SaveFactsheetRPC   = agrpc::ClientRPC<&dbv1::DispatchStorage::Stub::PrepareAsyncSaveFactsheet>;

constexpr auto kTimeout = std::chrono::seconds(3);      // один вызов базы
constexpr auto kRetryDelay = std::chrono::seconds(2);   // пауза, если база недоступна

bool is_unavailable(const grpc::Status& result) {
    return result.error_code() == grpc::StatusCode::UNAVAILABLE ||
           result.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED;
}

DatabaseWriter::DatabaseWriter(agrpc::GrpcContext& grpc_context, asio::io_context& io_context, const std::string& address)
    : grpc_context_(grpc_context),
      io_context_(io_context),
      stub_(dbv1::DispatchStorage::NewStub(grpc::CreateChannel(address, grpc::InsecureChannelCredentials()))),
      wakeup_(io_context, 1),
      retry_timer_(io_context) {}

void DatabaseWriter::start() {
    asio::co_spawn(io_context_, run(), [](std::exception_ptr error) {
        if (!error) return;
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            spdlog::error("[database] writer failed: {}", e.what());
        }
    });
}

void DatabaseWriter::stop() {
    stopped_ = true;
    wakeup_.close();
    retry_timer_.cancel();
}

void DatabaseWriter::save_status(const fms::v1::RobotStatus& status) {
    pending_[status.robot_id()] = status;
    wakeup_.try_send(boost::system::error_code{});
}

asio::awaitable<void> DatabaseWriter::run() {
    while (!stopped_) {
        if (pending_.empty()) {
            auto [ec] = co_await wakeup_.async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                break;   
            }
            continue;
        }

        auto batch = std::move(pending_);
        pending_.clear();
        bool retry = false;
        for (auto& [robot_id, status] : batch) {
            grpc::Status result = co_await send_status(status);
            report(robot_id, result);
            if (is_unavailable(result)) {
                pending_.try_emplace(robot_id, std::move(status));   // за это время мог прийти статус новее — он важнее
                retry = true;
            }
        }

        if (retry && !stopped_) {
            retry_timer_.expires_after(kRetryDelay);
            co_await retry_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        }
    }
}

asio::awaitable<grpc::Status> DatabaseWriter::send_status(const fms::v1::RobotStatus& status) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kTimeout);
    dbv1::SaveRobotStatusRequest request;
    *request.mutable_status() = status;
    dbv1::SaveRobotStatusResponse response;
    co_return co_await SaveRobotStatusRPC::request(grpc_context_, *stub_, context, request, response,
                                                    asio::use_awaitable);
}

asio::awaitable<bool> DatabaseWriter::save_factsheet(const std::string& robot_id, const std::string& json,
                                                     const google::protobuf::Timestamp& received_at) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kTimeout);
    dbv1::SaveFactsheetRequest request;
    request.set_robot_id(robot_id);
    request.set_factsheet_json(json);
    *request.mutable_received_at() = received_at;
    dbv1::SaveFactsheetResponse response;
    grpc::Status result = co_await SaveFactsheetRPC::request(grpc_context_, *stub_, context, request, response,
                                                             asio::use_awaitable);
    report(robot_id, result);
    co_return result.ok();
}

// Лог без спама: о пропаже базы и о незарегистрированном роботе — один раз, пока не изменится
void DatabaseWriter::report(const std::string& robot_id, const grpc::Status& result) {
    if (result.ok()) {
        if (!available_) {
            spdlog::info("[database] fleet_database is available again");
            available_ = true;
        }
        not_registered_.erase(robot_id);
    } else if (is_unavailable(result)) {
        if (available_) {
            spdlog::warn("[database] fleet_database unavailable ({}), retrying every {} s",
                         result.error_message(), kRetryDelay.count());
            available_ = false;
        }
    } else if (result.error_code() == grpc::StatusCode::NOT_FOUND) {
        if (not_registered_.insert(robot_id).second) {
            spdlog::warn("[database] robot {} is not registered in fleet_database, its data is not saved", robot_id);
        }
    } else {
        spdlog::error("[database] save for robot {} failed: {} {}", robot_id,
                      static_cast<int>(result.error_code()), result.error_message());
    }
}