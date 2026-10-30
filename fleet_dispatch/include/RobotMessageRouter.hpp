
#ifndef ROBOTMESSAGEROUTER_HPP
#define ROBOTMESSAGEROUTER_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

#include "FleetRegistry.hpp"
#include "RobotMessages.hpp"
#include "mqtt_client.hpp"

class RobotMessageRouter {
public:
    RobotMessageRouter(asio::io_context& io_context, MqttClientHandler& mqtt,
                       FleetRegistry& registry, Outbox& outbox);
    void start();

private:
    asio::awaitable<void> send_loop();
    asio::awaitable<void> receive_loop();

    asio::io_context& io_context_;
    MqttClientHandler& mqtt_;
    FleetRegistry& registry_;
    Outbox& outbox_;
};

#endif //ROBOTMESSAGEROUTER_HPP
