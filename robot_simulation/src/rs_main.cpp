#include <behaviortree_cpp/behavior_tree.h>
#include <print>
#include "include/RobotSimulator.hpp"
#include "include/TreeNodes.hpp"
#include "behaviortree_cpp/bt_factory.h"
#include <boost/asio/signal_set.hpp>

#include "include/RobotMessageRouter.hpp"
#include <yaml-cpp/yaml.h>
int main() {
    std::println("Robot simulation started");

    try {
        YAML::Node config = YAML::LoadFile("/home/ubuntu/fleet-manager/robot_simulation/config.yaml");

        std::string name = config["project_name"].as<std::string>();
        double version = config["version"].as<double>();
        bool is_enabled = config["enabled"].as<bool>();

        std::cout << "Project: " << name << " (v" << version << ")\n";
        std::cout << "Enabled: " << (is_enabled ? "Yes" : "No") << "\n\n";

        // 2. Чтение вложенных объектов (Map / Словник)
        if (config["database"]) {
            std::string db_host = config["database"]["host"].as<std::string>();
            int db_port = config["database"]["port"].as<int>();

            std::cout << "Database Settings:\n";
            std::cout << "  Host: " << db_host << "\n";
            std::cout << "  Port: " << db_port << "\n\n";
        }

        // 3. Чтение списков (Sequence / Массив)
        if (config["tags"] && config["tags"].IsSequence()) {
            std::cout << "Tags:\n";
            // Можно использовать обычный range-based for
            for (const auto& tag : config["tags"]) {
                std::cout << "  - " << tag.as<std::string>() << "\n";
            }
        }

    } catch (const YAML::BadFile& e) {
        std::cerr << "Ошибка: Не удалось открыть или найти файл config.yaml\n";
        return 1;
    } catch (const YAML::Exception& e) {
        std::cerr << "Ошибка парсинга YAML: " << e.what() << "\n";
        return 1;
    }

    return 0;

    // asio::io_context io_ctx;
    //
    // MqttClientConfig mqtt_config{ "0.0.0.0", 1883, "sim_client"};
    //
    // auto mqtt_client = std::make_shared<MqttClientHandler>(io_ctx, mqtt_config);
    // mqtt_client->start();
    //
    // auto publisher_channel = std::make_shared<publishing_channel>(io_ctx, 100);
    //
    // RobotMessageRouter Robots_router(mqtt_client, publisher_channel);
    //
    // BT::BehaviorTreeFactory factory;
    // factory.registerNodeType<WaitForOrder>("WaitForOrder");
    // factory.registerNodeType<GetNextSegment>("GetNextSegment");
    // factory.registerNodeType<CheckOrderValid>("CheckOrderValid");
    // factory.registerNodeType<MoveToPose>("MoveToPose");
    // factory.registerNodeType<ExecuteVdaActions>("ExecuteVdaActions");
    // factory.registerNodeType<MarkSegmentComplete>("MarkSegmentComplete");
    // factory.registerNodeType<OrderFullyCompleted>("OrderFullyCompleted");
    //
    // const int N = 1;
    //
    // for (int i = 0; i < N; ++i) {
    //     RobotConfig robot_config{"amr1", "KTC", "office"};
    //     auto robot = std::make_shared<RobotSimulator>(robot_config, publisher_channel);
    //
    //     robot->init_behavior_tree(factory,"/home/ubuntu/fleet-manager/robot_simulation/robot_tree.xml");
    //     robot->start(io_ctx.get_executor());
    //     Robots_router.register_robot(robot);
    // }
    //
    // Robots_router.start(io_ctx.get_executor());
    //
    // asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    // signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
    //     if (!error) {
    //         std::println("\n[System] Shutdown signal ({}) received! Stopping fleet gracefully...", signal_number);
    //         mqtt_client->stop();
    //         publisher_channel->cancel();
    //         Robots_router.shutdown();
    //         io_ctx.stop();
    //     }
    // });
    //
    // io_ctx.run();
    //
    // std::println("[System] Fleet Manager completely stopped. Memory released clean.");
    //
    // return 0;
}

