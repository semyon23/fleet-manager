#ifndef ROBOTOBJECT_HPP
#define ROBOTOBJECT_HPP

#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <optional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace fms {

enum class VDA5050AgvClass {
    FORKLIFT,
    CONVEYOR,
    TUGGER,
    CARRIER,
    MANIPULATOR,
    MOBILE_ROBOT
};

inline std::string to_string(VDA5050AgvClass val) {
    switch (val) {
        case VDA5050AgvClass::FORKLIFT:    return "FORKLIFT";
        case VDA5050AgvClass::CONVEYOR:    return "CONVEYOR";
        case VDA5050AgvClass::TUGGER:      return "TUGGER";
        case VDA5050AgvClass::CARRIER:     return "CARRIER";
        case VDA5050AgvClass::MANIPULATOR: return "MANIPULATOR";
        case VDA5050AgvClass::MOBILE_ROBOT:    return "MOBILE_ROBOT";
    }
    return "CARRIER";
}

enum class RobotState {
    IDLE,
    ERROR,
    ON_TASK,
    CHARGING,
    MAP_DEPLOYMENT,
    TELEOP
};

inline std::string to_string(RobotState val) {
    switch (val) {
        case RobotState::IDLE:           return "IDLE";
        case RobotState::ERROR:          return "ERROR";
        case RobotState::ON_TASK:        return "ON_TASK";
        case RobotState::CHARGING:       return "CHARGING";
        case RobotState::MAP_DEPLOYMENT: return "MAP_DEPLOYMENT";
        case RobotState::TELEOP:         return "TELEOP";
    }
    return "IDLE";
}

enum class RobotTeleopAction {
    START,
    STOP
};

inline std::string to_string(RobotTeleopAction val) {
    return (val == RobotTeleopAction::START) ? "START" : "STOP";
}

struct Pose2D {
    double x = 0.0;
    double y = 0.0;
    double theta = 0.0;
};


class RobotLogicError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};



struct RobotSoftwareVersion {
    std::string os = "";
    std::string app = "";
};

struct RobotHardwareVersion {
    std::string manufacturer = "";
    std::string serial_number = "";
};

class RobotTypeIdentifier {
private:
    std::string agv_class = "CARRIER";

public:
    double speed_max = -1.0;

    std::string get_agv_class() const { return agv_class; }

    void set_agv_class(std::string value) {
        if (value.empty()) {
            agv_class = "CARRIER";
            return;
        }

        std::string cleaned = value;
        cleaned.erase(cleaned.begin(), std::find_if(cleaned.begin(), cleaned.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        cleaned.erase(std::find_if(cleaned.rbegin(), cleaned.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), cleaned.end());

        std::transform(cleaned.begin(), cleaned.end(), cleaned.begin(), [](unsigned char c){ return std::toupper(c); });

        const std::vector<std::string> allowed = {"FORKLIFT", "CONVEYOR", "TUGGER", "CARRIER", "MANIPULATOR", "MOBILE_ROBOT"};
        if (std::find(allowed.begin(), allowed.end(), cleaned) != allowed.end()) {
            agv_class = cleaned;
        } else {
            throw std::invalid_argument("Invalid VDA5050AgvClass '" + value + "'. Allowed: FORKLIFT, CONVEYOR, TUGGER, CARRIER, MANIPULATOR, MOBILE_ROBOT");
        }
    }
};

struct RobotBatterySpec {
    double critical_level = 10.0;
    std::optional<double> recommended_minimum;
    std::optional<double> recommended_maximum;
};



struct RobotSpec {
    std::vector<std::string> labels;
    RobotBatterySpec battery;
    std::chrono::seconds heartbeat_timeout{30};
    bool switch_teleop = false;
};


struct RobotStatus {
    Pose2D pose;
    bool position_initialized = false;
    RobotSoftwareVersion software_version;
    RobotHardwareVersion hardware_version;
    RobotTypeIdentifier identifier;
    bool online = false;
    double battery_level = 0.0;
    RobotState state = RobotState::IDLE;
    json info_messages = json::object();
    json errors = json::object();
};

class RobotObject {
public:
    std::string name;
    RobotSpec spec;
    RobotStatus status;

    RobotObject() = default;
    explicit RobotObject(std::string robot_name) : name(std::move(robot_name)) {}

    bool is_running() const {
        return status.state == RobotState::ON_TASK ||
               status.state == RobotState::MAP_DEPLOYMENT ||
               status.state == RobotState::CHARGING;
    }

    bool can_switch_teleop() const {
        return status.state == RobotState::IDLE ||
               status.state == RobotState::ON_TASK ||
               status.state == RobotState::MAP_DEPLOYMENT ||
               status.state == RobotState::TELEOP;
    }

    bool can_deploy_map() const {
        return status.state == RobotState::IDLE ||
               status.state == RobotState::CHARGING;
    }

    // --- МЕТОДЫ УПРАВЛЕНИЯ (Бизнес-логика) ---
    /**
     * @brief Переключение режима телеуправления (Teleop)
     * @throws RobotLogicError если текущее состояние не позволяет переключить режим
     */
    std::string teleop(RobotTeleopAction action) {
        if (!can_switch_teleop()) {
            throw RobotLogicError(
                "Robot " + name + " is in state " + to_string(status.state) + " and request cannot be satisfied."
            );
        }

        spec.switch_teleop = (action == RobotTeleopAction::START);
        return to_string(action) + " teleop action received.";
    }

    static std::string get_alias() { return "robot"; }
};

} // namespace fleet


#endif //ROBOTOBJECT_HPP
