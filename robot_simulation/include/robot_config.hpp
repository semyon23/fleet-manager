#ifndef ROBOT_CONFIG_HPP
#define ROBOT_CONFIG_HPP

#pragma once

// Конфигурация симулируемого робота: по структуре на каждый компонент
// и общий RobotConfig, который RobotSimulator раздаёт компонентам.
// Загрузка из YAML — RobotConfigLoader.hpp.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

// Кто робот и как он называется в топиках VDA 5050:
// <interface>/<major_version>/<manufacturer>/<serial_number>/<topic>
struct IdentityConfig {
    std::string serial_number;
    std::string manufacturer;
    std::string interface_name = "amr";
    std::string major_version = "v3";

    std::string topic_prefix() const {
        return interface_name + "/" + major_version + "/" + manufacturer + "/" + serial_number + "/";
    }
};

struct MqttConfig {
    std::string host = "localhost";
    std::uint16_t port = 1883;
    std::string client_id;                // пусто — <manufacturer>_<serial_number>
    std::string username;
    std::string password;
};

struct InitialStateConfig {
    std::string map_id = "map_1";
    std::string map_version = "1";
    double x = 0.0;                       // [m]
    double y = 0.0;                       // [m]
    double theta = 0.0;                   // [rad]
    double battery_soc = 100.0;           // [%]
};

struct PublisherConfig {
    std::chrono::milliseconds min_state_interval{100};
    std::chrono::milliseconds max_state_interval{30'000};
    std::chrono::milliseconds visualization_interval{200};
};

// Контроллер движения (RobotDriveController).
struct MotionConfig {
    double max_speed = 1.0;               // [m/s]
    double max_accel = 0.5;               // [m/s^2] разгон и торможение
    double max_omega = 1.0;               // [rad/s]
    double k_omega = 3.0;                 // коэффициент поворота на цель
    double switch_radius = 0.25;          // [m] радиус переключения на следующую вершину
    double min_corner_speed = 0.1;        // [m/s] скорость в самом крутом повороте
    double lin_tol = 0.02;                // [m] точность в вершине-остановке
    double ang_tol = 0.01;                // [rad] точность ориентации
};

// Обработчики pick/drop (PickDropActions).
struct LoadHandlingConfig {
    double pick_time = 5.0;               // [s], как loadSets.pickTime в factsheet
    double drop_time = 5.0;               // [s], как loadSets.dropTime
    double time_jitter = 0.2;             // ±20% случайного разброса длительности
    double failure_probability = 0.0;     // вероятность FAILED — проверять реакцию fms
    std::uint32_t seed = 42;
    std::string lhd = "LHD1";             // имя грузозахвата (loadPositions)
    std::string load_type = "EPAL";
    double max_load_mass = 500.0;         // [kg]
};

// Физика и геометрия для factsheet (build_factsheet).
struct PhysicalConfig {
    std::string series_name = "MecanumBot";
    std::string series_description = "Simulated robot";
    std::string kinematics = "DIFFERENTIAL";         // DIFFERENTIAL | OMNIDIRECTIONAL | THREE_WHEEL
    std::string robot_class = "CARRIER";             // FORKLIFT | CONVEYOR | TUGGER | CARRIER
    std::string navigation_type = "VIRTUAL_LINE_GUIDED";
    double length = 0.8;                  // [m]
    double width = 0.6;                   // [m]
    double min_height = 0.25;             // [m]
    double max_height = 0.4;              // [m]
    std::string software_version = "0.1.0";
};

// Дерево поведения (BehaviorRunner).
struct BehaviorTreeConfig {
    std::string xml_path{ROBOT_TREE_XML};
    bool groot_enabled = true;
    std::optional<std::uint16_t> groot_port;   // пусто — назначит загрузчик (1667, 1669, ...)
};

// Цикл симуляции.
struct SimulationConfig {
    std::chrono::milliseconds tick_period{100};
    double battery_drain_per_meter = 0.01;     // [% / m]
};

struct RobotConfig {
    IdentityConfig identity;
    MqttConfig mqtt;
    InitialStateConfig initial_state;
    PublisherConfig publisher;
    MotionConfig motion;
    LoadHandlingConfig load_handling;
    PhysicalConfig physical;
    BehaviorTreeConfig behavior_tree;
    SimulationConfig simulation;

    const std::string& name() const { return identity.serial_number; }
};

#endif //ROBOT_CONFIG_HPP
