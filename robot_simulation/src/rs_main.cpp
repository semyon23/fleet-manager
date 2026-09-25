#include <behaviortree_cpp/behavior_tree.h>
#include <print>
#include "include/RobotSimulator.hpp"
#include "include/TreeNodes.hpp"
#include "behaviortree_cpp/bt_factory.h"
#include <boost/asio/signal_set.hpp>

#include "include/RobotMessageRouter.hpp"

int main() {
    std::println("Robot simulation started");
    asio::io_context io_ctx;

    MqttClientConfig mqtt_config{ "0.0.0.0", 1883, "sim_client"};

    auto mqtt_client = std::make_shared<MqttClientHandler>(io_ctx, mqtt_config);
    mqtt_client->start();

    auto publisher_channel = std::make_shared<publishing_channel>(io_ctx, 100);

    RobotMessageRouter Robots_router(mqtt_client, publisher_channel);

    BT::BehaviorTreeFactory factory;
    factory.registerNodeType<WaitForOrder>("WaitForOrder");
    factory.registerNodeType<GetNextSegment>("GetNextSegment");
    factory.registerNodeType<CheckOrderValid>("CheckOrderValid");
    factory.registerNodeType<MoveToPose>("MoveToPose");
    factory.registerNodeType<ExecuteVdaActions>("ExecuteVdaActions");
    factory.registerNodeType<MarkSegmentComplete>("MarkSegmentComplete");
    factory.registerNodeType<OrderFullyCompleted>("OrderFullyCompleted");

    const int N = 1;

    for (int i = 0; i < N; ++i) {
        RobotConfig robot_config{"amr1", "KTC", "office"};
        auto robot = std::make_shared<RobotSimulator>(robot_config, publisher_channel);

        robot->init_behavior_tree(factory,"/home/ubuntu/fleet-manager/robot_simulation/robot_tree.xml");
        robot->start(io_ctx.get_executor());
        Robots_router.register_robot(robot);
    }

    Robots_router.start(io_ctx.get_executor());

    asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
        if (!error) {
            std::println("\n[System] Shutdown signal ({}) received! Stopping fleet gracefully...", signal_number);
            mqtt_client->stop();
            publisher_channel->cancel();
            Robots_router.shutdown();
            io_ctx.stop();
        }
    });

    io_ctx.run();

    std::println("[System] Fleet Manager completely stopped. Memory released clean.");

    return 0;
}

