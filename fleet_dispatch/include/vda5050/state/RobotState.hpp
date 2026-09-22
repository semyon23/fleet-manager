#ifndef FLEETMANAGERSYSTEM_ROBOTSTATE_HPP
#define FLEETMANAGERSYSTEM_ROBOTSTATE_HPP

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <print>

namespace sentinel {
    constexpr double NO_VALUE = -1.0;
    constexpr double NO_ANGLE = -999.0;
    constexpr uint8_t NO_PROGRESS = 255;
}

enum class OperatingMode { AUTOMATIC, SEMIAUTOMATIC, MANUAL, SERVICE, TEACHIN, EMERGENCY };
enum class BlockingType { NONE, SOFT, HARD };
enum class ErrorLevel { WARNING, FATAL };

struct Position {
    double x = 0.0;
    double y = 0.0;
    double theta = 0.0;
    std::string mapId;
    bool positionInitialized = false;
    double deviationRange = sentinel::NO_VALUE;
};

struct Velocity {
    double vx = 0.0;
    double vy = 0.0;
    double omega = 0.0;
};

struct PlannedPathPoint {
    double x = 0.0;
    double y = 0.0;
    double theta = sentinel::NO_ANGLE; // -999.0 означает, что угол для этой точки пути не задан
};

struct BatteryState {
    double batteryCharge = 0.0;
    double batteryVoltage = sentinel::NO_VALUE;
    double batteryTemperature = sentinel::NO_VALUE;
    bool charging = false;
    bool powerSavingMode = false;
};

struct Error {
    std::string errorType;
    ErrorLevel errorLevel = ErrorLevel::WARNING;
    std::string errorDescription;
};

struct ActionState {
    std::string actionId;
    std::string actionType;
    std::string actionStatus;
    uint8_t progress = sentinel::NO_PROGRESS;
    std::string resultDescription;
};

struct EdgeState {
    std::string edgeId;
    uint32_t sequenceId = 0;
    bool released = false;
};

struct NodeState {
    std::string nodeId;
    uint32_t sequenceId = 0;
    bool released = false;
};

struct ZoneState {
    std::string zoneId;
    bool active = false;
};


struct RobotState {
    uint32_t headerId = 0;
    std::string timestamp;
    std::string version = "3.0.0";

    std::string orderId;
    uint32_t orderUpdateId = 0;
    uint32_t lastNodeSequenceId = 0;
    std::string lastNodeId;

    Position edgePosition;
    Velocity velocity;
    std::vector<PlannedPathPoint> plannedPath;

    OperatingMode operatingMode = OperatingMode::AUTOMATIC;
    BlockingType blockingType = BlockingType::NONE;
    bool idle = true;

    BatteryState batteryState;
    bool driving = false;
    bool paused = false;
    bool newBaseRequest = false;

    std::vector<NodeState> nodeStates;
    std::vector<EdgeState> edgeStates;
    std::vector<ActionState> actionStates;
    std::vector<ZoneState> zoneStates;
    std::vector<Error> errors;
};


NLOHMANN_JSON_SERIALIZE_ENUM(OperatingMode, {
    {OperatingMode::AUTOMATIC, "AUTOMATIC"},
    {OperatingMode::SEMIAUTOMATIC, "SEMIAUTOMATIC"},
    {OperatingMode::MANUAL, "MANUAL"},
    {OperatingMode::SERVICE, "SERVICE"},
    {OperatingMode::TEACHIN, "TEACHIN"},
    {OperatingMode::EMERGENCY, "EMERGENCY"}
})

NLOHMANN_JSON_SERIALIZE_ENUM(BlockingType, {
    {BlockingType::NONE, "NONE"},
    {BlockingType::SOFT, "SOFT"},
    {BlockingType::HARD, "HARD"}
})

NLOHMANN_JSON_SERIALIZE_ENUM(ErrorLevel, {
    {ErrorLevel::WARNING, "WARNING"},
    {ErrorLevel::FATAL, "FATAL"}
})

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Position, x, y, theta, mapId, positionInitialized, deviationRange)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Velocity, vx, vy, omega)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PlannedPathPoint, x, y, theta)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BatteryState, batteryCharge, batteryVoltage, batteryTemperature, charging, powerSavingMode)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Error, errorType, errorLevel, errorDescription)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ActionState, actionId, actionType, actionStatus, progress, resultDescription)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NodeState, nodeId, sequenceId, released)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EdgeState, edgeId, sequenceId, released)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ZoneState, zoneId, active)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RobotState,
    headerId, timestamp, version,
    orderId, orderUpdateId, lastNodeSequenceId, lastNodeId,
    edgePosition, velocity, plannedPath,
    operatingMode, blockingType, idle,
    batteryState, driving, paused, newBaseRequest,
    nodeStates, edgeStates, actionStates, zoneStates, errors
)

#endif //FLEETMANAGERSYSTEM_ROBOTSTATE_HPP
