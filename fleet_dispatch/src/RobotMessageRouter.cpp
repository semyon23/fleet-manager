
#include "../include/RobotMessageRouter.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "../include/VdaTopics.hpp"
#include "spdlog/spdlog.h"

namespace {
void log_if_failed(std::exception_ptr error) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        spdlog::error("[message_router] failed: {}", e.what());
    }
}

}

RobotMessageRouter::RobotMessageRouter(asio::io_context& io_context, MqttClientHandler& mqtt,
                                       FleetRegistry& registry, Outbox& outbox)
    : io_context_(io_context), mqtt_(mqtt), registry_(registry), outbox_(outbox) {}

void RobotMessageRouter::start() {
    asio::co_spawn(io_context_, send_loop(), log_if_failed);
    asio::co_spawn(io_context_, receive_loop(), log_if_failed);
}

asio::awaitable<void> RobotMessageRouter::send_loop() {
    spdlog::info("[message_router] send loop started");
    while (true) {
        auto [ec, request] = co_await outbox_.async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            break;   // очередь закрыта: остановка dispatch
        }
        switch (request.action) {
            case MqttAction::Publish:     mqtt_.publish_topic(request.topic, request.payload); break;
            case MqttAction::Subscribe:   mqtt_.subscribe_topic(request.topic); break;
            case MqttAction::Unsubscribe: mqtt_.unsubscribe_topic(request.topic); break;
        }
    }
    spdlog::info("[message_router] send loop stopped");
}

asio::awaitable<void> RobotMessageRouter::receive_loop() {
    spdlog::info("[message_router] receive loop started");
    while (true) {
        auto message = co_await mqtt_.get_message_received();
        if (!message) {
            break;
        }

        auto topic = parse_topic(message->topic);
        if (!topic) {
            spdlog::warn("[message_router] unexpected topic: {}", message->topic);
            continue;
        }

        if (!registry_.post(topic->manufacturer, topic->serial_number,
                            VdaMessage{std::move(message->topic), std::move(message->payload)})) {

            spdlog::debug("[message_router] message for {}/{} dropped", topic->manufacturer, topic->serial_number);
        }
    }
    spdlog::info("[message_router] receive loop stopped");
}