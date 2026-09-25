
#include <utility>

#include "../include/RobotMessageRouter.hpp"
#include <boost/asio/as_tuple.hpp>
RobotMessageRouter::RobotMessageRouter(std::shared_ptr<MqttClientHandler> client, Pub_channel_ptr channel)
    : mqtt_client(std::move(client)), publisher_channel(std::move(channel)){}

RobotMessageRouter::~RobotMessageRouter(){
    std::println("[RobotMessageRouter] Destruct");
}

void RobotMessageRouter::register_robot(const std::shared_ptr<RobotSimulator>& robot){
    robots_map[robot->get_name()] = robot;
    auto topics = robot->get_subscribe_topics();
    for (auto topic : topics){
        mqtt_client->subscribe_topic(topic);
        std::println("[RobotMessageRouter] Registered robot '{}' on topic '{}'", robot->get_name(), topic);
    }
}

void RobotMessageRouter::shutdown(){
    for (auto [_ , robot] : robots_map){
        robot->stop();
    }
}

void RobotMessageRouter::start(const asio::any_io_executor& executor){
    boost::asio::co_spawn(executor, listener_loop(), boost::asio::detached);
    boost::asio::co_spawn(executor, publisher_loop(), boost::asio::detached);
}

asio::awaitable<void> RobotMessageRouter::listener_loop(){
    while (true) {
        auto opt_msg = co_await mqtt_client->get_message_received();

        if (opt_msg.has_value()){
            const auto&  msg = opt_msg.value();
            auto it = robots_map.find(msg.topic);
            if (it != robots_map.end()) {
                it->second->update_order(msg.payload);
            }
        }else{
            co_return;
        }
    }
}

asio::awaitable<void> RobotMessageRouter::publisher_loop(){
    while (true) {
        auto [ec, msg]  = co_await publisher_channel->async_receive(asio::as_tuple(asio::deferred));
        if (ec) co_return;
        mqtt_client->publish_topic(msg.topic, msg.payload);
    }
}
