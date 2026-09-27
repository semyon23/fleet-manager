#pragma once

// VDA 5050 v3.0.0 — топик `order` (6.1, 7.3).
// Публикует: fleet control. Подписан: робот. QoS 0.
//
// Order — это сегмент графа: nodes[] и edges[] с общим sequenceId
// (node 0, edge 1, node 2, ...). edges.size() == nodes.size() - 1.
// released = true — "base" (можно ехать), false — "horizon" (план, ехать нельзя).
// Order update: тот же orderId, orderUpdateId + 1, первый node = последний node прошлой base.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "types.hpp"

namespace vda5050 {

struct Node {
    std::string node_id;
    std::uint32_t sequence_id = 0;               // чётные: 0, 2, 4, ...
    std::optional<std::string> node_descriptor;
    bool released = false;
    std::optional<NodePosition> node_position;   // можно не слать line-guided роботам
    std::vector<Action> actions;                 // пустой массив, если действий нет
};

inline void to_json(json& j, const Node& v) {
    j = json::object();
    detail::put(j, "nodeId", v.node_id);
    detail::put(j, "sequenceId", v.sequence_id);
    detail::put(j, "nodeDescriptor", v.node_descriptor);
    detail::put(j, "released", v.released);
    detail::put(j, "nodePosition", v.node_position);
    detail::put(j, "actions", v.actions);
}

inline void from_json(const json& j, Node& v) {
    detail::get(j, "nodeId", v.node_id);
    detail::get(j, "sequenceId", v.sequence_id);
    detail::get(j, "nodeDescriptor", v.node_descriptor);
    detail::get(j, "released", v.released);
    detail::get(j, "nodePosition", v.node_position);
    detail::get(j, "actions", v.actions);
}

// 'GLOBAL' — ориентация в системе координат карты (только омни-роботы),
// 'TANGENTIAL' — относительно касательной к траектории (0 — вперёд, Pi — назад). Default.
enum class OrientationType { Global, Tangential };
VDA5050_JSON_ENUM(OrientationType, {
    {OrientationType::Global, "GLOBAL"},
    {OrientationType::Tangential, "TANGENTIAL"},
})

enum class CorridorReferencePoint { KinematicCenter, Contour };
VDA5050_JSON_ENUM(CorridorReferencePoint, {
    {CorridorReferencePoint::KinematicCenter, "KINEMATIC_CENTER"},
    {CorridorReferencePoint::Contour, "CONTOUR"},
})

// Что делать, если разрешение на коридор истекло или отозвано.
enum class CorridorReleaseLossBehavior { Stop, Return };
VDA5050_JSON_ENUM(CorridorReleaseLossBehavior, {
    {CorridorReleaseLossBehavior::Stop, "STOP"},
    {CorridorReleaseLossBehavior::Return, "RETURN"},
})

// Коридор вокруг траектории ребра для объезда препятствий (6.1.5).
struct Corridor {
    double left_width = 0.0;                                         // [m]
    double right_width = 0.0;                                        // [m]
    std::optional<CorridorReferencePoint> corridor_reference_point;  // default KINEMATIC_CENTER
    std::optional<bool> release_required;                            // default false; true -> робот шлёт edgeRequest
    std::optional<CorridorReleaseLossBehavior> release_loss_behavior;  // default STOP
};

inline void to_json(json& j, const Corridor& v) {
    j = json::object();
    detail::put(j, "leftWidth", v.left_width);
    detail::put(j, "rightWidth", v.right_width);
    detail::put(j, "corridorReferencePoint", v.corridor_reference_point);
    detail::put(j, "releaseRequired", v.release_required);
    detail::put(j, "releaseLossBehavior", v.release_loss_behavior);
}

inline void from_json(const json& j, Corridor& v) {
    detail::get(j, "leftWidth", v.left_width);
    detail::get(j, "rightWidth", v.right_width);
    detail::get(j, "corridorReferencePoint", v.corridor_reference_point);
    detail::get(j, "releaseRequired", v.release_required);
    detail::get(j, "releaseLossBehavior", v.release_loss_behavior);
}

struct Edge {
    std::string edge_id;
    std::uint32_t sequence_id = 0;                         // нечётные: 1, 3, 5, ...
    std::optional<std::string> edge_descriptor;
    bool released = false;
    std::optional<double> maximum_speed;                   // [m/s]
    std::optional<double> maximum_mobile_robot_height;     // [m] вместе с грузом
    std::optional<double> minimum_load_handling_device_height;  // [m]
    std::optional<double> orientation;                     // [rad]
    std::optional<OrientationType> orientation_type;       // default TANGENTIAL
    std::optional<std::string> direction;                  // для физически line-guided: "left", "580 Hz", ...
    std::optional<bool> reach_orientation_before_entering; // только омни, default false
    std::optional<double> maximum_rotation_speed;          // [rad/s], нет — без ограничения
    std::optional<Trajectory> trajectory;
    std::optional<double> length;                          // [m]
    std::optional<Corridor> corridor;
    std::vector<Action> actions;                           // пустой массив, если действий нет
};

inline void to_json(json& j, const Edge& v) {
    j = json::object();
    detail::put(j, "edgeId", v.edge_id);
    detail::put(j, "sequenceId", v.sequence_id);
    detail::put(j, "edgeDescriptor", v.edge_descriptor);
    detail::put(j, "released", v.released);
    detail::put(j, "maximumSpeed", v.maximum_speed);
    detail::put(j, "maximumMobileRobotHeight", v.maximum_mobile_robot_height);
    detail::put(j, "minimumLoadHandlingDeviceHeight", v.minimum_load_handling_device_height);
    detail::put(j, "orientation", v.orientation);
    detail::put(j, "orientationType", v.orientation_type);
    detail::put(j, "direction", v.direction);
    detail::put(j, "reachOrientationBeforeEntering", v.reach_orientation_before_entering);
    detail::put(j, "maximumRotationSpeed", v.maximum_rotation_speed);
    detail::put(j, "trajectory", v.trajectory);
    detail::put(j, "length", v.length);
    detail::put(j, "corridor", v.corridor);
    detail::put(j, "actions", v.actions);
}

inline void from_json(const json& j, Edge& v) {
    detail::get(j, "edgeId", v.edge_id);
    detail::get(j, "sequenceId", v.sequence_id);
    detail::get(j, "edgeDescriptor", v.edge_descriptor);
    detail::get(j, "released", v.released);
    detail::get(j, "maximumSpeed", v.maximum_speed);
    detail::get(j, "maximumMobileRobotHeight", v.maximum_mobile_robot_height);
    detail::get(j, "minimumLoadHandlingDeviceHeight", v.minimum_load_handling_device_height);
    detail::get(j, "orientation", v.orientation);
    detail::get(j, "orientationType", v.orientation_type);
    detail::get(j, "direction", v.direction);
    detail::get(j, "reachOrientationBeforeEntering", v.reach_orientation_before_entering);
    detail::get(j, "maximumRotationSpeed", v.maximum_rotation_speed);
    detail::get(j, "trajectory", v.trajectory);
    detail::get(j, "length", v.length);
    detail::get(j, "corridor", v.corridor);
    detail::get(j, "actions", v.actions);
}

struct Order {
    Header header;
    std::string order_id;
    std::uint32_t order_update_id = 0;
    std::optional<std::string> order_description;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
};

inline void to_json(json& j, const Order& v) {
    j = json::object();
    put_header(j, v.header);
    detail::put(j, "orderId", v.order_id);
    detail::put(j, "orderUpdateId", v.order_update_id);
    detail::put(j, "orderDescription", v.order_description);
    detail::put(j, "nodes", v.nodes);
    detail::put(j, "edges", v.edges);
}

inline void from_json(const json& j, Order& v) {
    get_header(j, v.header);
    detail::get(j, "orderId", v.order_id);
    detail::get(j, "orderUpdateId", v.order_update_id);
    detail::get(j, "orderDescription", v.order_description);
    detail::get(j, "nodes", v.nodes);
    detail::get(j, "edges", v.edges);
}

}  // namespace vda5050
