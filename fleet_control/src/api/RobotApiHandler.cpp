#include "include/api/RobotApiHandler.hpp"
#include <chrono>

RobotApiHandler::RobotApiHandler(std::shared_ptr<fms::RobotsService> robotsServ,
                     std::shared_ptr<FleetDispatchClient> dispatchClient)
    : robots_service(std::move(robotsServ)), dispatch_client(std::move(dispatchClient)) {}

asio::awaitable<http::message_generator> RobotApiHandler::onCreateRobot(http::request<http::string_body>&& req){

    std::optional<http::message_generator> err_resp;

    auto body_opt = parse_json(req, err_resp);
    if (!body_opt){
        co_return std::move(err_resp.value());
    }

    auto& body = body_opt.value();
    unsigned version = req.version();

    if (!body.contains("name") || !body.contains("manufacturer")) {
        co_return reply_error(http::status::bad_request, version, "Missing 'name' or 'manufacturer'");
    }

    std::string name = body["name"];
    std::string manufacturer = body["manufacturer"];
    std::string agv_class = body.value("amr_class", "CARRIER");

    fms::RobotObject robot(name);

    try {
        robot.status.identifier.set_agv_class(agv_class);
    } catch (const std::invalid_argument& e) {
        co_return reply_error(http::status::bad_request, version, e.what());
    }


    if (!robots_service->add_robot(robot)) {
        co_return reply_error(http::status::conflict, version, "Robot already exists");
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    auto reg_resp = co_await dispatch_client->register_robot(name, manufacturer);

    auto end_time = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> elapsed_ms = end_time - start_time;

    if (reg_resp.success()) {
        std::cout << "[Scenario] Robot registered successfully! "
                  << "Response time: " << elapsed_ms.count() << " ms\n";
    } else {
        std::cout << "[Scenario] Registration failed: " << reg_resp.error_message()
                  << " (Took: " << elapsed_ms.count() << " ms)\n";
    }

    // if (reg_resp.success()) {
    //     std::cout << "[Scenario] Robot registered successfully!\n";
    // } else {
    //     std::cout << "[Scenario] Registration failed: " << reg_resp.error_message() << "\n";
    // }

    co_return reply_json(http::status::created, version, json{
        {"status", "success"},
        {"robot_id", name}
    });
}

asio::awaitable<http::message_generator> RobotApiHandler::onDeleteRobot(http::request<http::string_body>&& req){
    std::optional<http::message_generator> err_resp;
    unsigned version = req.version();

    auto body_opt = parse_json(req, err_resp);
    if (!body_opt) {
        co_return std::move(err_resp.value());
    }

    auto& body = *body_opt;

    if (!body.contains("name")) {
        co_return reply_error(http::status::bad_request, version, "Missing required fields: name or manufacturer");
    }

    std::string robot_name = body["name"];

    auto robot_opt = robots_service->get_robot(robot_name);
    if (!robot_opt) {
        co_return reply_error(http::status::not_found, version, "Robot '" + robot_name + "' not found in FMS");
    }

    if (robot_opt->is_running()) {
        co_return reply_error(
            http::status::conflict,
            version,
            "Cannot delete robot '" + robot_name + "' because it is currently active in state: " + to_string(robot_opt->status.state)
        );
    }

    auto reg_resp = co_await dispatch_client->unregister_robot(robot_name);

    if (reg_resp.success()) {
        std::cout << "[Scenario] Robot unregister_robot successfully!\n";
    } else {
        std::cout << "[Scenario] unregister_robot failed: " << "\n";
    }


    bool removed = robots_service->remove_robot(robot_name);
    if (!removed) {
        co_return reply_error(http::status::internal_server_error, version, "Internal error during memory cleanup");
    }

    co_return reply_json(http::status::ok, version, json{
        {"status", "success"},
        {"message", "Robot '" + robot_name + "' successfully unregistered from FMS and MQTT topics"}
    });
}

asio::awaitable<http::message_generator> RobotApiHandler::getAllRobots(http::request<http::string_body>&& req){
     unsigned version = req.version();


    std::vector<std::string> robot_names = robots_service->get_all_robot_names();

    json json_robots_array = json::array();

    for (const auto& name : robot_names) {
        auto robot_opt = robots_service->get_robot(name);
        if (!robot_opt) continue;

        auto& robot = *robot_opt;
        json robot_json;
        robot_json["name"] = robot.name;
        robot_json["spec"]["labels"] = robot.spec.labels;
        robot_json["spec"]["battery"]["critical_level"] = robot.spec.battery.critical_level;
        robot_json["spec"]["heartbeat_timeout_seconds"] = robot.spec.heartbeat_timeout.count();
        robot_json["spec"]["switch_teleop"] = robot.spec.switch_teleop;

        robot_json["status"]["online"] = robot.status.online;
        robot_json["status"]["state"] = to_string(robot.status.state);
        robot_json["status"]["battery_level"] = robot.status.battery_level;
        robot_json["status"]["position_initialized"] = robot.status.position_initialized;

        robot_json["status"]["pose"]["x"] = robot.status.pose.x;
        robot_json["status"]["pose"]["y"] = robot.status.pose.y;
        robot_json["status"]["pose"]["theta"] = robot.status.pose.theta;

        robot_json["status"]["identifier"]["agv_class"] = robot.status.identifier.get_agv_class();
        robot_json["status"]["identifier"]["speed_max"] = robot.status.identifier.speed_max;

        robot_json["status"]["software_version"]["os"] = robot.status.software_version.os;
        robot_json["status"]["software_version"]["app"] = robot.status.software_version.app;
        robot_json["status"]["hardware_version"]["manufacturer"] = robot.status.hardware_version.manufacturer;
        robot_json["status"]["hardware_version"]["serial_number"] = robot.status.hardware_version.serial_number;

        robot_json["status"]["info_messages"] = robot.status.info_messages;
        robot_json["status"]["errors"] = robot.status.errors;

        json_robots_array.push_back(robot_json);
    }

    co_return reply_json(http::status::ok, version, json_robots_array);
}
