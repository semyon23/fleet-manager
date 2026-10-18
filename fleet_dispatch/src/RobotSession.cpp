#include "../include/RobotSession.hpp"


#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "spdlog/spdlog.h"

RobotSession::RobotSession(asio::io_context& io_context, std::string robot_id,
                           std::string manufacturer, std::string serial_number)
    : io_context_(io_context),
      robot_id_(std::move(robot_id)),
      manufacturer_(std::move(manufacturer)),
      serial_number_(std::move(serial_number)),
      inbox_(io_context, 100) {
    status_.set_robot_id(robot_id_);
    status_.set_connection(fms::v1::CONNECTION_STATE_UNKNOWN);
}

void RobotSession::start() {
    asio::co_spawn(io_context_, [self = shared_from_this()] { return self->run(); },
                   [id = robot_id_](std::exception_ptr error) {
                       if (!error) return;
                       try {
                           std::rethrow_exception(error);
                       } catch (const std::exception& e) {
                           spdlog::error("[{}] session failed: {}", id, e.what());
                       }
                   });
}

void RobotSession::stop() {
    inbox_.close();
}

bool RobotSession::post(RobotMessage message) {
    return inbox_.try_send(boost::system::error_code{}, std::move(message));
}

asio::awaitable<void> RobotSession::run() {
    spdlog::info("[{}] session started ({} / {})", robot_id_, manufacturer_, serial_number_);
    while (true) {
        auto [ec, message] = co_await inbox_.async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            break;
        }
        spdlog::info("[{}] new message, topic:{} ", robot_id_, message.topic);
    }
    spdlog::info("[{}] session stopped", robot_id_);
}