#include "../include/FleetStreamGateway.hpp"
#include <print>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

FleetStreamGateway::FleetStreamGateway(MqttClientHandler& mqtt, std::shared_ptr<GrpcServerHandler> grpc_server)
        : mqtt_client(mqtt), server(grpc_server){}


void FleetStreamGateway::run(asio::io_context& ctx){
    asio::co_spawn(ctx, [self = shared_from_this()]() {
            return self->mqtt_reader_loop();
        }, asio::detached);

    asio::co_spawn(ctx, [self = shared_from_this()]() {
           return self->handle_registration_events();
       }, asio::detached);
}

asio::awaitable<void> FleetStreamGateway::handle_registration_events(){
    std::println("[Gateway] Started gRPC registration event processor loop.");

    while (true) {
        auto reg_event = co_await server->get_register_robot();

        if (reg_event.has_value()) {
            const auto& robot = reg_event.value();
            std::println("[Gateway] Processing registration for robot: {}", robot.robot_id);

            std::string status_topic = "uagv/v2/" + robot.manufacturer + "/" + robot.robot_id + "/status";
            std::string visu_topic = "uagv/v2/" + robot.manufacturer + "/" + robot.robot_id + "/visualization";

            mqtt_client.subscribe_topic(status_topic);
            mqtt_client.subscribe_topic(visu_topic);
        } else {
            std::println("[Gateway] gRPC registration channel closed. Exiting loop.");
            asio::steady_timer timer(co_await boost::asio::this_coro::executor, std::chrono::seconds(2));
            co_await timer.async_wait(boost::asio::use_awaitable);
        }
    }
}

asio::awaitable<void> FleetStreamGateway::handle_unregistration_events(){
    std::println("[Gateway] Started gRPC unregistration event processor loop.");

    while (true) {
        auto unreg_event = co_await server->get_unregister_robot();

        if (unreg_event.has_value()) {
            const auto& robot = unreg_event.value();
            std::println("[Gateway] Processing unregistration for robot: {}", robot.robot_id);

            std::string status_topic = "uagv/v2/" + robot.manufacturer + "/" + robot.robot_id + "/status";
            std::string visu_topic = "uagv/v2/" + robot.manufacturer + "/" + robot.robot_id + "/visualization";

            mqtt_client.unsubscribe_topic(status_topic);
            mqtt_client.unsubscribe_topic(visu_topic);
        } else {
            std::println("[Gateway] gRPC registration channel closed. Exiting loop.");
            asio::steady_timer timer(co_await boost::asio::this_coro::executor, std::chrono::seconds(2));
            co_await timer.async_wait(boost::asio::use_awaitable);
        }
    }
}


asio::awaitable<void> FleetStreamGateway::mqtt_reader_loop(){
     while (true) {
        auto msg = co_await mqtt_client.get_message_received();

        if (!msg.has_value()) {
            std::println("MQTT message channel closed. Exiting reader loop.");
            break;
        }

        auto [topic, payload] = std::move(msg.value());

        try {
            auto data = json::parse(payload);

            if (topic.find("state") != std::string::npos)
            {
                RobotStatusUpdate status;
                status.robot_id = data.value("serialNumber", "");

                status.status = data.value("operatingMode", "IDLE");
                status.operating_mode = data.value("operatingMode", "AUTOMATIC");
                status.timestamp = data.value("timestamp", "");
                status.driving = data.value("driving", false);

                if (data.contains("batteryState")) {
                    auto& b = data["batteryState"];
                    status.battery_state.battery_charge = b.value("batteryCharge", 0.0);
                    status.battery_state.charging = b.value("charging", false);
                }

                server->set_status_robot(status);
            }
            else if (topic.find("visualization") != std::string::npos)
            {
                RobotVisualization visu;
                visu.robot_id = data.value("serialNumber", "");

                if (data.contains("agvPosition")) {
                    auto& p = data["agvPosition"];
                    visu.agv_position.map_id = p.value("mapId", "");
                    visu.agv_position.x = p.value("x", 0.0);
                    visu.agv_position.y = p.value("y", 0.0);
                    visu.agv_position.theta = p.value("theta", 0.0);
                    visu.agv_position.position_initialized = p.value("positionInitialized", false);
                }

                server->set_visu_robot(visu);

            }
        }
        catch (const json::parse_error& e) {
            std::cerr << "[JSON Error] Failed to parse MQTT payload: " << e.what() << "\n";
        }
        catch (const std::exception& e) {
            std::cerr << "[Error] Unexpected error in reader loop: " << e.what() << "\n";
        }
    }
}


