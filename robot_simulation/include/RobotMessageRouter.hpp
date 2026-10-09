#ifndef FLEETMANAGERSYSTEM_ROBOTMESSAGEROUTER_HPP
#define FLEETMANAGERSYSTEM_ROBOTMESSAGEROUTER_HPP

#include <memory>
#include <string>
#include <unordered_map>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include "include/RoboticVehicleSimulator.hpp"
#include "../shared/include/mqtt_client.hpp"



class RobotMessageRouter : public std::enable_shared_from_this<RobotMessageRouter> {
public:
    RobotMessageRouter(std::shared_ptr<MqttClientHandler> client, ptrPublishingChannel channel);
    void register_robot(const std::vector<TopicHandler>& routes);
    void start(const asio::any_io_executor& executor);
private:
    asio::awaitable<void> publisher_loop();
    asio::awaitable<void> listener_loop();
    std::shared_ptr<MqttClientHandler> mqtt_client;
    ptrPublishingChannel publisher_channel;

    std::unordered_map<std::string, std::function<void(const std::string&)>> message_map;
};

#endif //FLEETMANAGERSYSTEM_ROBOTMESSAGEROUTER_HPP
