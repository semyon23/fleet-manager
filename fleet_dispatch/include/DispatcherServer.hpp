
#ifndef FLEETSTREAMGATEWAY_HPP
#define FLEETSTREAMGATEWAY_HPP

#include "include/MqttClientHandler.hpp"
#include "include/GrpcDispatchService.hpp"
#include "controllers/RobotTopicManager.hpp"

class DispatcherServer : public std::enable_shared_from_this<DispatcherServer>  {
public:
    DispatcherServer(MqttClientHandler& mqtt, std::shared_ptr<GrpcDispatchService> grpc_server);
    void run(asio::io_context& ctx);

private:
    asio::awaitable<void> mqtt_reader_loop();
    asio::awaitable<void> handle_registration_events();
    asio::awaitable<void> handle_unregistration_events();
    MqttClientHandler& mqtt_client;
    std::shared_ptr<GrpcDispatchService> server;

    RobotTopicManager robot_topic_manager;
};



#endif //FLEETSTREAMGATEWAY_HPP
