#ifndef FLEETMANAGERSYSTEM_VISUALIZATION_HPP
#define FLEETMANAGERSYSTEM_VISUALIZATION_HPP
#pragma once
#include "../state/State.hpp"

struct ControlPoint {
    double x = 0.0;     // [m] X-coordinate
    double y = 0.0;     // [m] Y-coordinate
    double weight = 0.0; // Вес контрольной точки, по дефолту 1.0
};

struct Trajectory {
    uint32_t degree = 0.0; // Степень кривой NURBS, по дефолту 1
    std::vector<double> knotVector;             // Узловой вектор (массив float64 от 0.0 до 1.0)
    std::vector<ControlPoint> controlPoints;    // Контрольные точки кривой
};

struct PlannedPath {
    Trajectory trajectory;                      // Математическое описание траектории NURBS
    std::vector<std::string> traversedNodes;    // Массив IDs нод из текущего ордера, пересекаемых этим путем
};

struct Waypoint {
    double x = 0.0;     // [m] X-coordinate
    double y = 0.0;     // [m] Y-coordinate
    double theta = 0.0; // [rad] Absolute orientation [-Pi ... Pi]
    std::string eta;    // Estimated time of arrival (ISO 8601 UTC, e.g. "2026-09-23T10:28:00.123Z")
};

struct IntermediatePath {
    std::vector<Waypoint> polyline; // Массив отрезков ломаной линии
};

struct VisualizationMessage {
    uint32_t headerId = 0;
    std::string timestamp;
    std::string version = "3.0.0";
    std::string manufacturer;
    std::string serialNumber;
    uint32_t referenceStateHeaderId;

    PlannedPath plannedPath;
    IntermediatePath intermediatePath;

    MobileRobotPosition position;
    Velocity velocity;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Waypoint, x, y, theta, eta)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(IntermediatePath, polyline)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ControlPoint, x, y, weight)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Trajectory, degree, knotVector, controlPoints)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PlannedPath, trajectory, traversedNodes)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(VisualizationMessage,
    headerId, timestamp, version, manufacturer, serialNumber, referenceStateHeaderId,
    position, velocity, plannedPath, intermediatePath
)



#endif //FLEETMANAGERSYSTEM_VISUALIZATION_HPP
