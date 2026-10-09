
#include <utility>
#include "../include/RobotMessageRouter.hpp"
#include <boost/asio/as_tuple.hpp>
#include <spdlog/spdlog.h>
RobotMessageRouter::RobotMessageRouter(std::shared_ptr<MqttClientHandler> client, ptrPublishingChannel channel)
    : mqtt_client(std::move(client)), publisher_channel(std::move(channel)){}

void RobotMessageRouter::register_robot(const std::vector<TopicHandler>& routes){
    for (const auto& route : routes) {
        message_map[route.topic] = route.handler;
        mqtt_client->subscribe_topic(route.topic);
        spdlog::info("[message_router] Successfully routed topic: '{}'", route.topic);
    }
}


void RobotMessageRouter::start(const asio::any_io_executor& executor){
    boost::asio::co_spawn(executor, listener_loop(), boost::asio::detached);
    boost::asio::co_spawn(executor, publisher_loop(), boost::asio::detached);
}

asio::awaitable<void> RobotMessageRouter::listener_loop()
{
    while (true){
        auto opt_msg = co_await mqtt_client->get_message_received();

        if (!opt_msg.has_value())
        {
            spdlog::error("[message_router] MQTT client disconnected or error occurred. Exiting listener loop.");
            co_return;
        }

        const auto& msg = opt_msg.value();
        auto it = message_map.find(msg.topic);

        if (it != message_map.end())
        {
            it->second(msg.payload);
            spdlog::info("[message_router] New message, topic - {}", msg.topic);
        }
    }
}

asio::awaitable<void> RobotMessageRouter::publisher_loop(){
    while (true) {
        auto [ec, msg]  = co_await publisher_channel->async_receive(asio::as_tuple(asio::deferred));
        if (ec) co_return;
        mqtt_client->publish_topic(msg.topic, msg.payload, msg.qos, msg.retained);
    }
}

