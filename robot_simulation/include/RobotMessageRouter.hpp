#ifndef FLEETMANAGERSYSTEM_ROBOTMESSAGEROUTER_HPP
#define FLEETMANAGERSYSTEM_ROBOTMESSAGEROUTER_HPP

#include <memory>
#include <string>
#include <unordered_map>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include "include/RobotSimulator.hpp"
#include "../shared/include/mqtt_client.hpp"



class RobotMessageRouter : public std::enable_shared_from_this<RobotMessageRouter> {
public:
    RobotMessageRouter(std::shared_ptr<MqttClientHandler> client, Pub_channel_ptr channel);

    ~RobotMessageRouter();

    void register_robot(const std::shared_ptr<RobotSimulator>& robot);
    std::string extract_robot_name(std::string_view topic);
    void start(const asio::any_io_executor& executor);
    void shutdown();



private:
    asio::awaitable<void> publisher_loop();
    asio::awaitable<void> listener_loop();
    std::shared_ptr<MqttClientHandler> mqtt_client;
    Pub_channel_ptr publisher_channel;

    std::unordered_map<std::string, std::shared_ptr<RobotSimulator>> robots_map;
};

#endif //FLEETMANAGERSYSTEM_ROBOTMESSAGEROUTER_HPP
