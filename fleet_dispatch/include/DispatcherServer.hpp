
#ifndef FLEETSTREAMGATEWAY_HPP
#define FLEETSTREAMGATEWAY_HPP

#include "../../shared/include/mqtt_client.hpp"
#include "include/GrpcDispatchService.hpp"
#include "controllers/RobotTopicManager.hpp"
#include "include/controllers/RobotController.hpp"
#include <unordered_map>

class DispatcherServer : public std::enable_shared_from_this<DispatcherServer>  {
public:
    DispatcherServer(MqttClientHandler& mqtt, std::shared_ptr<GrpcDispatchService> grpc_server);
    void run(asio::io_context& ctx);

private:
    asio::awaitable<void> mqtt_reader_loop();
    asio::awaitable<void> handle_registration_robots();
    asio::awaitable<void> handle_unregistration_robots();
    MqttClientHandler& mqtt_client;
    std::shared_ptr<GrpcDispatchService> server;

    RobotTopicManager robot_topic_manager;

    std::unordered_map<std::string, std::shared_ptr<RobotController>> robot_controllers;
};



#endif //FLEETSTREAMGATEWAY_HPP
