
#ifndef FLEETMANAGERSYSTEM_CONFIGLOADER_HPP
#define FLEETMANAGERSYSTEM_CONFIGLOADER_HPP
#pragma once

#include <yaml-cpp/yaml.h>
#include "include/robot/robot_config.hpp"
class ConfigLoader {

public:
    static AppConfig load(const std::string& file_path) {
        YAML::Node config;
        try {
            config = YAML::LoadFile(file_path);
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to load YAML file: " + std::string(e.what()));
        }

        AppConfig app_config;

        app_config.mqtt = parse_mqtt(config["mqtt"]);


        InitialStateConfig common_initial_state = parse_initial_state(config["initial_state"]);
        PublisherConfig common_publisher = parse_publisher(config["publisher"]);
        MotionConfig common_motion = parse_motion(config["motion"]);
        LoadHandlingConfig common_load_handling = parse_load_handling(config["load_handling"]);
        PhysicalConfig common_physical = parse_physical(config["physical"]);
        BehaviorTreeConfig common_behavior_tree = parse_behavior_tree(config["behavior_tree"]);
        SimulationConfig common_simulation = parse_simulation(config["simulation"]);
        BatteryConfig common_battery_config = parse_battery_config(config["battery_config"]);
        if (!config["robots"]) {
            throw std::runtime_error("Configuration error: Top-level key 'robots' is missing in YAML file.");
        }

        if (!config["robots"].IsSequence()) {
            throw std::runtime_error("Configuration error: 'robots' section must be a YAML sequence (list starting with '-').");
        }

        for (const auto& robot_node : config["robots"]){
            RobotConfig rc{};

            if (!robot_node["serial_number"]) {
                throw std::runtime_error("Configuration error: Each robot in the 'robots' list MUST have a 'serial_number'.");
            }
            rc.identity.serial_number = robot_node["serial_number"].as<std::string>();

            if (robot_node["manufacturer"]) rc.identity.manufacturer = robot_node["manufacturer"].as<std::string>();
            if (robot_node["interface_name"]) rc.identity.interface_name = robot_node["interface_name"].as<std::string>();
            if (robot_node["major_version"])  rc.identity.major_version  = robot_node["major_version"].as<std::string>();


            rc.initial_state = parse_section_or_default(robot_node, "initial_state", common_initial_state, parse_initial_state);
            rc.publisher     = parse_section_or_default(robot_node, "publisher",     common_publisher,     parse_publisher);
            rc.motion        = parse_section_or_default(robot_node, "motion",        common_motion,        parse_motion);
            rc.load_handling = parse_section_or_default(robot_node, "load_handling", common_load_handling, parse_load_handling);
            rc.physical      = parse_section_or_default(robot_node, "physical",      common_physical,      parse_physical);
            rc.behavior_tree = parse_section_or_default(robot_node, "behavior_tree", common_behavior_tree, parse_behavior_tree);
            rc.simulation    = parse_section_or_default(robot_node, "simulation",    common_simulation,    parse_simulation);
            rc.battery       = parse_section_or_default(robot_node, "battery_config",common_battery_config,parse_battery_config);
            app_config.robots.push_back(rc);
        }

        return app_config;
    }



private:
    static MqttConfig parse_mqtt(const YAML::Node& node) {
        MqttConfig c;
        if (!node) return c;
        if (node["host"])      c.host      = node["host"].as<std::string>();
        if (node["port"])      c.port      = node["port"].as<std::uint16_t>();
        if (node["client_id"]) c.client_id = node["client_id"].as<std::string>();
        if (node["username"])  c.username  = node["username"].as<std::string>();
        if (node["password"])  c.password  = node["password"].as<std::string>();
        return c;
    }

    static InitialStateConfig parse_initial_state(const YAML::Node& node) {
        InitialStateConfig c;
        if (!node) return c;
        if (node["map_id"])       c.map_id = node["map_id"].as<std::string>();
        if (node["map_version"])  c.map_version = node["map_version"].as<std::string>();
        if (node["x"])            c.x = node["x"].as<double>();
        if (node["y"])            c.y = node["y"].as<double>();
        if (node["theta"])        c.theta = node["theta"].as<double>();
        if (node["battery_soc"])  c.battery_soc = node["battery_soc"].as<double>();
        return c;
    }

    static BatteryConfig parse_battery_config(const YAML::Node& node){
        BatteryConfig c;
        if (!node) return c;
        if (node["drain_per_meter"])        c.drain_per_meter = node["drain_per_meter"].as<double>();
        if (node["drain_idle_per_s"])       c.drain_idle_per_s = node["drain_idle_per_s"].as<double>();
        if (node["charge_per_s"])           c.charge_per_s = node["charge_per_s"].as<double>();
        if (node["charging_start_time"])    c.charging_start_time = node["charging_start_time"].as<double>();
        if (node["voltage"])                c.voltage = node["voltage"].as<double>();
        if (node["critical_low_level"])     c.critical_low_level = node["critical_low_level"].as<double>();
        if (node["minimum_desired_level"])  c.minimum_desired_level = node["minimum_desired_level"].as<double>();
        if (node["maximum_desired_level"])  c.maximum_desired_level = node["maximum_desired_level"].as<double>();
        return c;
    }

    static PublisherConfig parse_publisher(const YAML::Node& node) {
        PublisherConfig c;
        if (!node) return c;
        if (node["min_state_interval_ms"])
            c.min_state_interval = std::chrono::milliseconds(node["min_state_interval_ms"].as<int64_t>());
        if (node["max_state_interval_ms"])
            c.max_state_interval = std::chrono::milliseconds(node["max_state_interval_ms"].as<int64_t>());
        if (node["visualization_interval_ms"])
            c.visualization_interval = std::chrono::milliseconds(node["visualization_interval_ms"].as<int64_t>());
        return c;
    }

    static MotionConfig parse_motion(const YAML::Node& node) {
        MotionConfig c;
        if (!node) return c;
        if (node["max_speed"])        c.max_speed = node["max_speed"].as<double>();
        if (node["max_accel"])        c.max_accel = node["max_accel"].as<double>();
        if (node["max_omega"])        c.max_omega = node["max_omega"].as<double>();
        if (node["k_omega"])          c.k_omega = node["k_omega"].as<double>();
        if (node["switch_radius"])    c.switch_radius = node["switch_radius"].as<double>();
        if (node["min_corner_speed"]) c.min_corner_speed = node["min_corner_speed"].as<double>();
        if (node["lin_tol"])          c.lin_tol = node["lin_tol"].as<double>();
        if (node["ang_tol"])          c.ang_tol = node["ang_tol"].as<double>();
        return c;
    }

    static LoadHandlingConfig parse_load_handling(const YAML::Node& node) {
        LoadHandlingConfig c;
        if (!node) return c;
        if (node["pick_time"])           c.pick_time = node["pick_time"].as<double>();
        if (node["drop_time"])           c.drop_time = node["drop_time"].as<double>();
        if (node["time_jitter"])         c.time_jitter = node["time_jitter"].as<double>();
        if (node["failure_probability"]) c.failure_probability = node["failure_probability"].as<double>();
        if (node["seed"])                c.seed = node["seed"].as<std::uint32_t>();
        if (node["lhd"])                 c.lhd = node["lhd"].as<std::string>();
        if (node["load_type"])           c.load_type = node["load_type"].as<std::string>();
        if (node["max_load_mass"])       c.max_load_mass = node["max_load_mass"].as<double>();
        return c;
    }

    static PhysicalConfig parse_physical(const YAML::Node& node) {
        PhysicalConfig c;
        if (!node) return c;
        if (node["series_name"])        c.series_name = node["series_name"].as<std::string>();
        if (node["series_description"]) c.series_description = node["series_description"].as<std::string>();
        if (node["kinematics"])         c.kinematics = node["kinematics"].as<std::string>();
        if (node["robot_class"])        c.robot_class = node["robot_class"].as<std::string>();
        if (node["navigation_type"])    c.navigation_type = node["navigation_type"].as<std::string>();
        if (node["length"])             c.length = node["length"].as<double>();
        if (node["width"])              c.width = node["width"].as<double>();
        if (node["min_height"])         c.min_height = node["min_height"].as<double>();
        if (node["max_height"])         c.max_height = node["max_height"].as<double>();
        if (node["software_version"])   c.software_version = node["software_version"].as<std::string>();
        return c;
    }

    static BehaviorTreeConfig parse_behavior_tree(const YAML::Node& node) {
        BehaviorTreeConfig c;
        if (!node) return c;
        if (node["xml_path"]) c.xml_path = node["xml_path"].as<std::string>();
        return c;
    }

    static SimulationConfig parse_simulation(const YAML::Node& node) {
        SimulationConfig c;
        if (!node) return c;
        if (node["tick_period_ms"])
            c.tick_period = std::chrono::milliseconds(node["tick_period_ms"].as<int64_t>());
        return c;
    }

    template <typename T, typename ParseFunc>
    static T parse_section_or_default(const YAML::Node& robot_node, const std::string& section_name,
                                      const T& default_value, ParseFunc parse_function){
        if (robot_node[section_name]){
            if (!robot_node[section_name].IsMap()){
                std::string robot_name = robot_node["serial_number"]
                                             ? robot_node["serial_number"].as<std::string>()
                                             : "unknown";
                throw std::runtime_error(
                    "Configuration error in robot '" + robot_name + "': '" + section_name +
                    "' must be a key-value map.");
            }

            return parse_function(robot_node[section_name]);
        }

        return default_value;
    }
};


#endif //FLEETMANAGERSYSTEM_CONFIGLOADER_HPP
