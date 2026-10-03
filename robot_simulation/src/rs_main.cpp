#include <behaviortree_cpp/behavior_tree.h>
#include <print>
#include "include/RobotSimulator.hpp"
#include "behaviortree_cpp/bt_factory.h"
#include <boost/asio/signal_set.hpp>

#include "include/RobotMessageRouter.hpp"
#include <yaml-cpp/yaml.h>
#include "../vda5050/vda5050.hpp"
#include "include/RobotDriveController.hpp"
#include "include/action_nodes.hpp"
#include "../shared/include/logger.hpp"

int main() {
    if (!Logger::init()) return 1;
    spdlog::info("[System] Robot simulation started");
    asio::io_context io_ctx;

    MqttClientConfig mqtt_config{ "0.0.0.0", 1883, "sim_client"};

    auto mqtt_client = std::make_shared<MqttClientHandler>(io_ctx, mqtt_config);
    mqtt_client->start();

    auto publisher_channel = std::make_shared<publishing_channel>(io_ctx, 100);

    RobotMessageRouter Robots_router(mqtt_client, publisher_channel);

    BT::BehaviorTreeFactory factory;
    register_action_nodes(factory);

    const int number_robots = 1;

    for (int i = 0; i < number_robots; ++i) {
        std::string robot_name = std::format("amr{}", i + 1);
        RobotConfig robot_config{robot_name, "KTC", "office"};
        auto robot = std::make_shared<RobotSimulator>(robot_config, publisher_channel);

        robot->init_behavior_tree(factory, ROBOT_TREE_XML);

        robot->start(io_ctx.get_executor());
        Robots_router.register_robot(robot);
    }

    Robots_router.start(io_ctx.get_executor());

    asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
        if (!error) {
            spdlog::info("[System] Shutdown signal ({}) received!", signal_number);
            mqtt_client->stop();
            publisher_channel->cancel();
            Robots_router.shutdown();
            io_ctx.stop();
        }
    });

    io_ctx.run();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return 0;
}

