#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"

namespace vda5050 {

// ============================================================================
// Actions (6.2, 7.3)
// ============================================================================

// 'NONE'   — можно ехать и выполнять другие action параллельно
// 'SINGLE' — можно ехать, но других action параллельно нельзя
// 'SOFT'   — ехать нельзя, другие action параллельно можно
// 'HARD'   — единственная разрешённая activity в этот момент
enum class BlockingType { None, Single, Soft, Hard };
VDA5050_JSON_ENUM(BlockingType, {
    {BlockingType::None, "NONE"},
    {BlockingType::Single, "SINGLE"},
    {BlockingType::Soft, "SOFT"},
    {BlockingType::Hard, "HARD"},
})

namespace action_type {
inline constexpr const char* kStartPause = "startPause";
inline constexpr const char* kStopPause = "stopPause";
inline constexpr const char* kStartHibernation = "startHibernation";
inline constexpr const char* kStopHibernation = "stopHibernation";
inline constexpr const char* kShutdown = "shutdown";
inline constexpr const char* kStartCharging = "startCharging";
inline constexpr const char* kStopCharging = "stopCharging";
inline constexpr const char* kInitializePosition = "initializePosition";
inline constexpr const char* kEnableMap = "enableMap";
inline constexpr const char* kDownloadMap = "downloadMap";
inline constexpr const char* kDeleteMap = "deleteMap";
inline constexpr const char* kDownloadZoneSet = "downloadZoneSet";
inline constexpr const char* kEnableZoneSet = "enableZoneSet";
inline constexpr const char* kDeleteZoneSet = "deleteZoneSet";
inline constexpr const char* kClearInstantActions = "clearInstantActions";
inline constexpr const char* kClearZoneActions = "clearZoneActions";
inline constexpr const char* kStateRequest = "stateRequest";
inline constexpr const char* kLogReport = "logReport";
inline constexpr const char* kPick = "pick";
inline constexpr const char* kDrop = "drop";
inline constexpr const char* kDetectObject = "detectObject";
inline constexpr const char* kFinePositioning = "finePositioning";
inline constexpr const char* kWaitForTrigger = "waitForTrigger";
inline constexpr const char* kTrigger = "trigger";
inline constexpr const char* kRetry = "retry";
inline constexpr const char* kSkipRetry = "skipRetry";
inline constexpr const char* kCancelOrder = "cancelOrder";
inline constexpr const char* kFactsheetRequest = "factsheetRequest";
inline constexpr const char* kUpdateCertificate = "updateCertificate";
}  // namespace action_type

// Параметр action (7.3.1): {"key": "...", "value": <любой JSON>}.
struct ActionParameter {
    std::string key;
    json value;  // array / boolean / number / integer / string / object
};

inline void to_json(json& j, const ActionParameter& v) {
    j = json::object();
    detail::put(j, "key", v.key);
    detail::put(j, "value", v.value);
}

inline void from_json(const json& j, ActionParameter& v) {
    detail::get(j, "key", v.key);
    detail::get(j, "value", v.value);
}

// Action в order (на node/edge) и в instantActions.
struct Action {
    std::string action_type;                                     // см. action_type::*
    std::string action_id;                                       // уникальный, рекомендуется UUID
    std::optional<std::string> action_descriptor;                // только для людей, не для логики
    BlockingType blocking_type = BlockingType::None;             // у instantAction всегда NONE
    std::optional<std::vector<ActionParameter>> action_parameters;
    std::optional<bool> retriable;                               // default false
};

inline void to_json(json& j, const Action& v) {
    j = json::object();
    detail::put(j, "actionType", v.action_type);
    detail::put(j, "actionId", v.action_id);
    detail::put(j, "actionDescriptor", v.action_descriptor);
    detail::put(j, "blockingType", v.blocking_type);
    detail::put(j, "actionParameters", v.action_parameters);
    detail::put(j, "retriable", v.retriable);
}

inline void from_json(const json& j, Action& v) {
    detail::get(j, "actionType", v.action_type);
    detail::get(j, "actionId", v.action_id);
    detail::get(j, "actionDescriptor", v.action_descriptor);
    detail::get(j, "blockingType", v.blocking_type);
    detail::get(j, "actionParameters", v.action_parameters);
    detail::get(j, "retriable", v.retriable);
}

// ============================================================================
// Позиции узлов (7.3)
// ============================================================================

// Допустимое отклонение от узла — эллипс вокруг позиции узла (6.6.2.1).
// a = b = 0 — отклонение не допускается.
struct AllowedDeviationXY {
    double a = 0.0;      // [m] большая полуось
    double b = 0.0;      // [m] малая полуось
    double theta = 0.0;  // [rad] поворот большой оси относительно оси X карты
};

inline auto eq_key(const AllowedDeviationXY& v) {
    return std::tie(v.a, v.b, v.theta);
}

inline void to_json(json& j, const AllowedDeviationXY& v) {
    j = json::object();
    detail::put(j, "a", v.a);
    detail::put(j, "b", v.b);
    detail::put(j, "theta", v.theta);
}

inline void from_json(const json& j, AllowedDeviationXY& v) {
    detail::get(j, "a", v.a);
    detail::get(j, "b", v.b);
    detail::get(j, "theta", v.theta);
}

// Позиция узла. Одна структура и для order.node, и для state.nodeState:
// в state поля allowedDeviation* не присылаются (они остаются nullopt).
struct NodePosition {
    double x = 0.0;                                           // [m]
    double y = 0.0;                                           // [m]
    std::optional<double> theta;                              // [rad] [-Pi..Pi]
    std::optional<AllowedDeviationXY> allowed_deviation_xy;   // только в order
    std::optional<double> allowed_deviation_theta;            // [rad] [0..Pi], только в order
    std::string map_id;
};

inline auto eq_key(const NodePosition& v) {
    return std::tie(v.x, v.y, v.theta, v.allowed_deviation_xy, v.allowed_deviation_theta, v.map_id);
}

inline void to_json(json& j, const NodePosition& v) {
    j = json::object();
    detail::put(j, "x", v.x);
    detail::put(j, "y", v.y);
    detail::put(j, "theta", v.theta);
    detail::put(j, "allowedDeviationXY", v.allowed_deviation_xy);
    detail::put(j, "allowedDeviationTheta", v.allowed_deviation_theta);
    detail::put(j, "mapId", v.map_id);
}

inline void from_json(const json& j, NodePosition& v) {
    detail::get(j, "x", v.x);
    detail::get(j, "y", v.y);
    detail::get(j, "theta", v.theta);
    detail::get(j, "allowedDeviationXY", v.allowed_deviation_xy);
    detail::get(j, "allowedDeviationTheta", v.allowed_deviation_theta);
    detail::get(j, "mapId", v.map_id);
}

// ============================================================================
// Траектория NURBS (7.3)
// ============================================================================

struct ControlPoint {
    double x = 0.0;                 // [m]
    double y = 0.0;                 // [m]
    std::optional<double> weight;   // ]0..max], default 1.0
};

inline auto eq_key(const ControlPoint& v) {
    return std::tie(v.x, v.y, v.weight);
}

inline void to_json(json& j, const ControlPoint& v) {
    j = json::object();
    detail::put(j, "x", v.x);
    detail::put(j, "y", v.y);
    detail::put(j, "weight", v.weight);
}

inline void from_json(const json& j, ControlPoint& v) {
    detail::get(j, "x", v.x);
    detail::get(j, "y", v.y);
    detail::get(j, "weight", v.weight);
}

struct Trajectory {
    std::optional<std::uint32_t> degree;               // default 1
    std::optional<std::vector<double>> knot_vector;    // size = controlPoints.size() + degree + 1, значения [0..1]
    std::vector<ControlPoint> control_points;          // минимум degree + 1, включая начало и конец
};

inline auto eq_key(const Trajectory& v) {
    return std::tie(v.degree, v.knot_vector, v.control_points);
}

inline void to_json(json& j, const Trajectory& v) {
    j = json::object();
    detail::put(j, "degree", v.degree);
    detail::put(j, "knotVector", v.knot_vector);
    detail::put(j, "controlPoints", v.control_points);
}

inline void from_json(const json& j, Trajectory& v) {
    detail::get(j, "degree", v.degree);
    detail::get(j, "knotVector", v.knot_vector);
    detail::get(j, "controlPoints", v.control_points);
}

// ============================================================================
// Позиция и движение робота (7.8) — общие для state и visualization
// ============================================================================

struct MobileRobotPosition {
    double x = 0.0;                                // [m]
    double y = 0.0;                                // [m]
    double theta = 0.0;                            // [rad] [-Pi..Pi]
    std::string map_id;
    bool localized = false;                        // false -> x/y/theta не доверять
    std::optional<double> localization_score;      // [0..1], только для логов/визуализации
    std::optional<double> deviation_range;         // [m], только для логов/визуализации
};

inline void to_json(json& j, const MobileRobotPosition& v) {
    j = json::object();
    detail::put(j, "x", v.x);
    detail::put(j, "y", v.y);
    detail::put(j, "theta", v.theta);
    detail::put(j, "mapId", v.map_id);
    detail::put(j, "localized", v.localized);
    detail::put(j, "localizationScore", v.localization_score);
    detail::put(j, "deviationRange", v.deviation_range);
}

inline void from_json(const json& j, MobileRobotPosition& v) {
    detail::get(j, "x", v.x);
    detail::get(j, "y", v.y);
    detail::get(j, "theta", v.theta);
    detail::get(j, "mapId", v.map_id);
    detail::get(j, "localized", v.localized);
    detail::get(j, "localizationScore", v.localization_score);
    detail::get(j, "deviationRange", v.deviation_range);
}

// Скорость в системе координат робота.
struct Velocity {
    std::optional<double> vx;      // [m/s]
    std::optional<double> vy;      // [m/s]
    std::optional<double> omega;   // [rad/s]
};

inline void to_json(json& j, const Velocity& v) {
    j = json::object();
    detail::put(j, "vx", v.vx);
    detail::put(j, "vy", v.vy);
    detail::put(j, "omega", v.omega);
}

inline void from_json(const json& j, Velocity& v) {
    detail::get(j, "vx", v.vx);
    detail::get(j, "vy", v.vy);
    detail::get(j, "omega", v.omega);
}

// Путь, запланированный самим роботом в рамках текущего order (6.8).
struct PlannedPath {
    Trajectory trajectory;
    std::optional<std::vector<std::string>> traversed_nodes;  // nodeId из order
};

inline void to_json(json& j, const PlannedPath& v) {
    j = json::object();
    detail::put(j, "trajectory", v.trajectory);
    detail::put(j, "traversedNodes", v.traversed_nodes);
}

inline void from_json(const json& j, PlannedPath& v) {
    detail::get(j, "trajectory", v.trajectory);
    detail::get(j, "traversedNodes", v.traversed_nodes);
}

struct Waypoint {
    double x = 0.0;                // [m]
    double y = 0.0;                // [m]
    std::optional<double> theta;   // [rad]
    std::string eta;               // ISO 8601 UTC
};

inline void to_json(json& j, const Waypoint& v) {
    j = json::object();
    detail::put(j, "x", v.x);
    detail::put(j, "y", v.y);
    detail::put(j, "theta", v.theta);
    detail::put(j, "eta", v.eta);
}

inline void from_json(const json& j, Waypoint& v) {
    detail::get(j, "x", v.x);
    detail::get(j, "y", v.y);
    detail::get(j, "theta", v.theta);
    detail::get(j, "eta", v.eta);
}

// Ближний путь (то, что робот видит сенсорами) — ломаная с ETA, начинается от текущей позиции.
struct IntermediatePath {
    std::vector<Waypoint> polyline;
};

inline void to_json(json& j, const IntermediatePath& v) {
    j = json::object();
    detail::put(j, "polyline", v.polyline);
}

inline void from_json(const json& j, IntermediatePath& v) {
    detail::get(j, "polyline", v.polyline);
}

// ============================================================================
// Геометрия
// ============================================================================

// Вершина полигона (зоны, огибающие робота). Координаты в [m].
struct Vertex {
    double x = 0.0;
    double y = 0.0;
};

inline void to_json(json& j, const Vertex& v) {
    j = json::object();
    detail::put(j, "x", v.x);
    detail::put(j, "y", v.y);
}

inline void from_json(const json& j, Vertex& v) {
    detail::get(j, "x", v.x);
    detail::get(j, "y", v.y);
}

// Точка отсчёта bounding box груза: центр нижней грани, в системе координат робота.
struct BoundingBoxReference {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    std::optional<double> theta;
};

inline auto eq_key(const BoundingBoxReference& v) {
    return std::tie(v.x, v.y, v.z, v.theta);
}

inline void to_json(json& j, const BoundingBoxReference& v) {
    j = json::object();
    detail::put(j, "x", v.x);
    detail::put(j, "y", v.y);
    detail::put(j, "z", v.z);
    detail::put(j, "theta", v.theta);
}

inline void from_json(const json& j, BoundingBoxReference& v) {
    detail::get(j, "x", v.x);
    detail::get(j, "y", v.y);
    detail::get(j, "z", v.z);
    detail::get(j, "theta", v.theta);
}

struct LoadDimensions {
    double length = 0.0;             // [m] вдоль оси X робота
    double width = 0.0;              // [m] вдоль оси Y робота
    std::optional<double> height;    // [m]
};

inline auto eq_key(const LoadDimensions& v) {
    return std::tie(v.length, v.width, v.height);
}

inline void to_json(json& j, const LoadDimensions& v) {
    j = json::object();
    detail::put(j, "length", v.length);
    detail::put(j, "width", v.width);
    detail::put(j, "height", v.height);
}

inline void from_json(const json& j, LoadDimensions& v) {
    detail::get(j, "length", v.length);
    detail::get(j, "width", v.width);
    detail::get(j, "height", v.height);
}

// ============================================================================
// Типы зон (6.4.1) — в zoneSet и в factsheet.supportedZones
// ============================================================================

enum class ZoneType {
    Blocked,                // contour-based
    LineGuided,             // contour-based
    Release,                // contour-based, интерактивная (zoneRequest ACCESS)
    CoordinatedReplanning,  // contour-based, интерактивная (zoneRequest REPLANNING)
    SpeedLimit,             // contour-based
    Action,                 // contour-based
    Priority,               // kinematic center-based
    Penalty,                // kinematic center-based
    Directed,               // kinematic center-based
    Bidirected,             // kinematic center-based
};
VDA5050_JSON_ENUM(ZoneType, {
    {ZoneType::Blocked, "BLOCKED"},
    {ZoneType::LineGuided, "LINE_GUIDED"},
    {ZoneType::Release, "RELEASE"},
    {ZoneType::CoordinatedReplanning, "COORDINATED_REPLANNING"},
    {ZoneType::SpeedLimit, "SPEED_LIMIT"},
    {ZoneType::Action, "ACTION"},
    {ZoneType::Priority, "PRIORITY"},
    {ZoneType::Penalty, "PENALTY"},
    {ZoneType::Directed, "DIRECTED"},
    {ZoneType::Bidirected, "BIDIRECTED"},
})

}  // namespace vda5050
