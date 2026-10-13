#include <behaviortree_cpp/behavior_tree.h>
#include <print>
#include "behaviortree_cpp/bt_factory.h"
#include <boost/asio/signal_set.hpp>

#include "../include/app/RobotMessageRouter.hpp"
#include <yaml-cpp/yaml.h>

#include "include/behavior/action_nodes.hpp"
#include "../shared/include/logger.hpp"
#include "include/app/config_loader.hpp"


int main(){
    if (!Logger::init()) return 1;
    spdlog::info("[system] Robot simulation started");
    AppConfig app_config{};
    try{
        app_config = ConfigLoader::load("/home/ubuntu/CLionProjects/fleet-manager/robot_simulation/config/config.yaml");

        spdlog::info("[System][config] Mqtt broker address: "
                     "host {}:{}, (username = {}, password = {}) ", app_config.mqtt.host, app_config.mqtt.port, app_config.mqtt.username, app_config.mqtt.password);
        spdlog::info("[System][config] Total robots : {}", app_config.robots.size());

        for (auto& robot : app_config.robots){
            robot.identity.serial_number = robot.identity.interface_name + robot.identity.serial_number;
            spdlog::info("[System][config] Robot name: {}, "
                         "(initial pos x:{}, y:{}, t:{})", robot.identity.serial_number, robot.initial_state.x, robot.initial_state.y, robot.initial_state.theta);
        }
    } catch (const std::exception& e){
        spdlog::error("[System] Initialization failed: {}", e.what());
        return 1;
    }

    asio::io_context io_ctx;
    auto& [host, port, id, user, pass] = app_config.mqtt;
    MqttClientConfig mqtt_config{host, port, id, user, pass};

    auto mqtt_client = std::make_shared<MqttClientHandler>(io_ctx, mqtt_config);
    mqtt_client->start();

    auto publisher_channel = std::make_shared<publishing_channel>(io_ctx, 100);

    RobotMessageRouter Robots_router(mqtt_client, publisher_channel);

    BT::BehaviorTreeFactory factory;
    register_action_nodes(factory);

    const size_t number_robots{app_config.robots.size()};
    std::vector<std::shared_ptr<RoboticVehicleSimulator>> robots;
    robots.reserve(number_robots);

    for (int i = 0; i < number_robots; ++i) {
        RobotConfig robot_config{app_config.robots[i]};
        auto robot = std::make_shared<RoboticVehicleSimulator>(io_ctx.get_executor(), robot_config, publisher_channel);
        robot->init_behavior_tree(factory);
        robot->on_link_up();
        robot->start();
        Robots_router.register_robot(robot->get_topic_routes());
        robots.push_back(robot);
    }

    Robots_router.start(io_ctx.get_executor());

    asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
        if (!error) {
            spdlog::info("[System] Shutdown signal ({}) received!", signal_number);
            for (auto robot : robots){
                robot->stop();
            }
            mqtt_client->stop();
            publisher_channel->cancel();
           // io_ctx.stop();
        }
    });

    io_ctx.run();
    return 0;
}

