#pragma once

// VDA 5050 v3.0.0 — топик `factsheet` (6.10, 7.10).
// Публикует: робот. Подписан: fleet control. QoS 0, retained.
// Запросить заново — instant action factsheetRequest.
//
// Описание типа робота: класс, кинематика, габариты, лимиты протокола,
// поддерживаемые optional-поля и action, геометрия, грузы.
// Fleet control обязан слать роботу только те optional-поля, что он поддерживает (7.1.1).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "types.hpp"

namespace vda5050 {

// ============================================================================
// typeSpecification
// ============================================================================

// Расширяемые enum'ы — строки. Предопределённые значения:
namespace mobile_robot_kinematics {
inline constexpr const char* kDifferential = "DIFFERENTIAL";
inline constexpr const char* kOmnidirectional = "OMNIDIRECTIONAL";
inline constexpr const char* kThreeWheel = "THREE_WHEEL";
}  // namespace mobile_robot_kinematics

namespace mobile_robot_class {
inline constexpr const char* kForklift = "FORKLIFT";
inline constexpr const char* kConveyor = "CONVEYOR";
inline constexpr const char* kTugger = "TUGGER";
inline constexpr const char* kCarrier = "CARRIER";
}  // namespace mobile_robot_class

namespace localization_type {
inline constexpr const char* kNatural = "NATURAL";
inline constexpr const char* kReflector = "REFLECTOR";
inline constexpr const char* kRfid = "RFID";
inline constexpr const char* kDmc = "DMC";
inline constexpr const char* kSpot = "SPOT";
inline constexpr const char* kGrid = "GRID";
}  // namespace localization_type

namespace navigation_type {
inline constexpr const char* kPhysicalLineGuided = "PHYSICAL_LINE_GUIDED";
inline constexpr const char* kVirtualLineGuided = "VIRTUAL_LINE_GUIDED";
inline constexpr const char* kFreelyNavigating = "FREELY_NAVIGATING";
}  // namespace navigation_type

struct TypeSpecification {
    std::string series_name;
    std::optional<std::string> series_description;
    std::string mobile_robot_kinematics;           // см. mobile_robot_kinematics::*
    std::string mobile_robot_class;                // см. mobile_robot_class::*
    double maximum_load_mass = 0.0;                // [kg]
    std::vector<std::string> localization_types;   // см. localization_type::*
    std::vector<std::string> navigation_types;     // по приоритету, см. navigation_type::*
    std::optional<std::vector<ZoneType>> supported_zones;
};

inline void to_json(json& j, const TypeSpecification& v) {
    j = json::object();
    detail::put(j, "seriesName", v.series_name);
    detail::put(j, "seriesDescription", v.series_description);
    detail::put(j, "mobileRobotKinematics", v.mobile_robot_kinematics);
    detail::put(j, "mobileRobotClass", v.mobile_robot_class);
    detail::put(j, "maximumLoadMass", v.maximum_load_mass);
    detail::put(j, "localizationTypes", v.localization_types);
    detail::put(j, "navigationTypes", v.navigation_types);
    detail::put(j, "supportedZones", v.supported_zones);
}

inline void from_json(const json& j, TypeSpecification& v) {
    detail::get(j, "seriesName", v.series_name);
    detail::get(j, "seriesDescription", v.series_description);
    detail::get(j, "mobileRobotKinematics", v.mobile_robot_kinematics);
    detail::get(j, "mobileRobotClass", v.mobile_robot_class);
    detail::get(j, "maximumLoadMass", v.maximum_load_mass);
    detail::get(j, "localizationTypes", v.localization_types);
    detail::get(j, "navigationTypes", v.navigation_types);
    detail::get(j, "supportedZones", v.supported_zones);
}

// ============================================================================
// physicalParameters
// ============================================================================

struct PhysicalParameters {
    double minimum_speed = 0.0;                     // [m/s]
    double maximum_speed = 0.0;                     // [m/s]
    std::optional<double> minimum_angular_speed;    // [rad/s]
    std::optional<double> maximum_angular_speed;    // [rad/s]
    double maximum_acceleration = 0.0;              // [m/s^2] с максимальным грузом
    double maximum_deceleration = 0.0;              // [m/s^2] с максимальным грузом
    double minimum_height = 0.0;                    // [m]
    double maximum_height = 0.0;                    // [m]
    double width = 0.0;                             // [m]
    double length = 0.0;                            // [m]
};

inline void to_json(json& j, const PhysicalParameters& v) {
    j = json::object();
    detail::put(j, "minimumSpeed", v.minimum_speed);
    detail::put(j, "maximumSpeed", v.maximum_speed);
    detail::put(j, "minimumAngularSpeed", v.minimum_angular_speed);
    detail::put(j, "maximumAngularSpeed", v.maximum_angular_speed);
    detail::put(j, "maximumAcceleration", v.maximum_acceleration);
    detail::put(j, "maximumDeceleration", v.maximum_deceleration);
    detail::put(j, "minimumHeight", v.minimum_height);
    detail::put(j, "maximumHeight", v.maximum_height);
    detail::put(j, "width", v.width);
    detail::put(j, "length", v.length);
}

inline void from_json(const json& j, PhysicalParameters& v) {
    detail::get(j, "minimumSpeed", v.minimum_speed);
    detail::get(j, "maximumSpeed", v.maximum_speed);
    detail::get(j, "minimumAngularSpeed", v.minimum_angular_speed);
    detail::get(j, "maximumAngularSpeed", v.maximum_angular_speed);
    detail::get(j, "maximumAcceleration", v.maximum_acceleration);
    detail::get(j, "maximumDeceleration", v.maximum_deceleration);
    detail::get(j, "minimumHeight", v.minimum_height);
    detail::get(j, "maximumHeight", v.maximum_height);
    detail::get(j, "width", v.width);
    detail::get(j, "length", v.length);
}

// ============================================================================
// protocolLimits. Не задано или 0 — ограничения нет.
// ============================================================================

struct MaximumStringLengths {
    std::optional<std::uint32_t> maximum_message_length;
    std::optional<std::uint32_t> maximum_topic_serial_length;
    std::optional<std::uint32_t> maximum_topic_element_length;
    std::optional<std::uint32_t> maximum_id_length;
    std::optional<bool> id_numerical_only;  // true — все Id только из цифр
    std::optional<std::uint32_t> maximum_load_id_length;
};

inline void to_json(json& j, const MaximumStringLengths& v) {
    j = json::object();
    detail::put(j, "maximumMessageLength", v.maximum_message_length);
    detail::put(j, "maximumTopicSerialLength", v.maximum_topic_serial_length);
    detail::put(j, "maximumTopicElementLength", v.maximum_topic_element_length);
    detail::put(j, "maximumIdLength", v.maximum_id_length);
    detail::put(j, "idNumericalOnly", v.id_numerical_only);
    detail::put(j, "maximumLoadIdLength", v.maximum_load_id_length);
}

inline void from_json(const json& j, MaximumStringLengths& v) {
    detail::get(j, "maximumMessageLength", v.maximum_message_length);
    detail::get(j, "maximumTopicSerialLength", v.maximum_topic_serial_length);
    detail::get(j, "maximumTopicElementLength", v.maximum_topic_element_length);
    detail::get(j, "maximumIdLength", v.maximum_id_length);
    detail::get(j, "idNumericalOnly", v.id_numerical_only);
    detail::get(j, "maximumLoadIdLength", v.maximum_load_id_length);
}

// Ключи в JSON содержат точку: "order.nodes", "state.errors", ...
struct MaximumArrayLengths {
    std::optional<std::uint32_t> order_nodes;
    std::optional<std::uint32_t> order_edges;
    std::optional<std::uint32_t> node_actions;
    std::optional<std::uint32_t> edge_actions;
    std::optional<std::uint32_t> actions_actions_parameters;
    std::optional<std::uint32_t> instant_actions;
    std::optional<std::uint32_t> trajectory_knot_vector;
    std::optional<std::uint32_t> trajectory_control_points;
    std::optional<std::uint32_t> zone_set_zones;
    std::optional<std::uint32_t> state_node_states;
    std::optional<std::uint32_t> state_edge_states;
    std::optional<std::uint32_t> state_loads;
    std::optional<std::uint32_t> state_action_states;
    std::optional<std::uint32_t> state_instant_action_states;
    std::optional<std::uint32_t> state_zone_action_states;
    std::optional<std::uint32_t> state_errors;
    std::optional<std::uint32_t> state_information;
    std::optional<std::uint32_t> error_error_references;
    std::optional<std::uint32_t> information_info_references;
};

inline void to_json(json& j, const MaximumArrayLengths& v) {
    j = json::object();
    detail::put(j, "order.nodes", v.order_nodes);
    detail::put(j, "order.edges", v.order_edges);
    detail::put(j, "node.actions", v.node_actions);
    detail::put(j, "edge.actions", v.edge_actions);
    detail::put(j, "actions.actionsParameters", v.actions_actions_parameters);
    detail::put(j, "instantActions", v.instant_actions);
    detail::put(j, "trajectory.knotVector", v.trajectory_knot_vector);
    detail::put(j, "trajectory.controlPoints", v.trajectory_control_points);
    detail::put(j, "zoneSet.zones", v.zone_set_zones);
    detail::put(j, "state.nodeStates", v.state_node_states);
    detail::put(j, "state.edgeStates", v.state_edge_states);
    detail::put(j, "state.loads", v.state_loads);
    detail::put(j, "state.actionStates", v.state_action_states);
    detail::put(j, "state.instantActionStates", v.state_instant_action_states);
    detail::put(j, "state.zoneActionStates", v.state_zone_action_states);
    detail::put(j, "state.errors", v.state_errors);
    detail::put(j, "state.information", v.state_information);
    detail::put(j, "error.errorReferences", v.error_error_references);
    detail::put(j, "information.infoReferences", v.information_info_references);
}

inline void from_json(const json& j, MaximumArrayLengths& v) {
    detail::get(j, "order.nodes", v.order_nodes);
    detail::get(j, "order.edges", v.order_edges);
    detail::get(j, "node.actions", v.node_actions);
    detail::get(j, "edge.actions", v.edge_actions);
    detail::get(j, "actions.actionsParameters", v.actions_actions_parameters);
    detail::get(j, "instantActions", v.instant_actions);
    detail::get(j, "trajectory.knotVector", v.trajectory_knot_vector);
    detail::get(j, "trajectory.controlPoints", v.trajectory_control_points);
    detail::get(j, "zoneSet.zones", v.zone_set_zones);
    detail::get(j, "state.nodeStates", v.state_node_states);
    detail::get(j, "state.edgeStates", v.state_edge_states);
    detail::get(j, "state.loads", v.state_loads);
    detail::get(j, "state.actionStates", v.state_action_states);
    detail::get(j, "state.instantActionStates", v.state_instant_action_states);
    detail::get(j, "state.zoneActionStates", v.state_zone_action_states);
    detail::get(j, "state.errors", v.state_errors);
    detail::get(j, "state.information", v.state_information);
    detail::get(j, "error.errorReferences", v.error_error_references);
    detail::get(j, "information.infoReferences", v.information_info_references);
}

struct Timing {
    double minimum_order_interval = 0.0f;                 // [s] не слать order чаще
    double minimum_state_interval = 0.0f;                 // [s]
    std::optional<float> default_state_interval;         // [s], нет — 30 с из спецификации
    std::optional<float> visualization_interval;         // [s]
};

inline void to_json(json& j, const Timing& v) {
    j = json::object();
    detail::put(j, "minimumOrderInterval", v.minimum_order_interval);
    detail::put(j, "minimumStateInterval", v.minimum_state_interval);
    detail::put(j, "defaultStateInterval", v.default_state_interval);
    detail::put(j, "visualizationInterval", v.visualization_interval);
}

inline void from_json(const json& j, Timing& v) {
    detail::get(j, "minimumOrderInterval", v.minimum_order_interval);
    detail::get(j, "minimumStateInterval", v.minimum_state_interval);
    detail::get(j, "defaultStateInterval", v.default_state_interval);
    detail::get(j, "visualizationInterval", v.visualization_interval);
}

struct ProtocolLimits {
    MaximumStringLengths maximum_string_lengths;
    MaximumArrayLengths maximum_array_lengths;
    Timing timing;
};

inline void to_json(json& j, const ProtocolLimits& v) {
    j = json::object();
    detail::put(j, "maximumStringLengths", v.maximum_string_lengths);
    detail::put(j, "maximumArrayLengths", v.maximum_array_lengths);
    detail::put(j, "timing", v.timing);
}

inline void from_json(const json& j, ProtocolLimits& v) {
    detail::get(j, "maximumStringLengths", v.maximum_string_lengths);
    detail::get(j, "maximumArrayLengths", v.maximum_array_lengths);
    detail::get(j, "timing", v.timing);
}

// ============================================================================
// protocolFeatures
// ============================================================================

enum class Support {
    Supported,  // optional-параметр поддерживается
    Required,   // без него робот нормально не работает
};
VDA5050_JSON_ENUM(Support, {
    {Support::Supported, "SUPPORTED"},
    {Support::Required, "REQUIRED"},
})

struct OptionalParameter {
    std::string parameter;  // полное имя, напр. "order.nodes.nodePosition.allowedDeviationTheta"
    Support support = Support::Supported;
    std::optional<std::string> description;
};

inline void to_json(json& j, const OptionalParameter& v) {
    j = json::object();
    detail::put(j, "parameter", v.parameter);
    detail::put(j, "support", v.support);
    detail::put(j, "description", v.description);
}

inline void from_json(const json& j, OptionalParameter& v) {
    detail::get(j, "parameter", v.parameter);
    detail::get(j, "support", v.support);
    detail::get(j, "description", v.description);
}

enum class ActionScope { Instant, Node, Edge, Zone };
VDA5050_JSON_ENUM(ActionScope, {
    {ActionScope::Instant, "INSTANT"},
    {ActionScope::Node, "NODE"},
    {ActionScope::Edge, "EDGE"},
    {ActionScope::Zone, "ZONE"},
})

enum class ValueDataType { Bool, Number, Integer, String, Object, Array };
VDA5050_JSON_ENUM(ValueDataType, {
    {ValueDataType::Bool, "BOOL"},
    {ValueDataType::Number, "NUMBER"},
    {ValueDataType::Integer, "INTEGER"},
    {ValueDataType::String, "STRING"},
    {ValueDataType::Object, "OBJECT"},
    {ValueDataType::Array, "ARRAY"},
})

// Описание параметра action в factsheet — это НЕ ActionParameter из order (там key/value).
struct FactsheetActionParameter {
    std::string key;
    ValueDataType value_data_type = ValueDataType::String;
    std::optional<std::string> description;
    std::optional<bool> is_optional;
};

inline void to_json(json& j, const FactsheetActionParameter& v) {
    j = json::object();
    detail::put(j, "key", v.key);
    detail::put(j, "valueDataType", v.value_data_type);
    detail::put(j, "description", v.description);
    detail::put(j, "isOptional", v.is_optional);
}

inline void from_json(const json& j, FactsheetActionParameter& v) {
    detail::get(j, "key", v.key);
    detail::get(j, "valueDataType", v.value_data_type);
    detail::get(j, "description", v.description);
    detail::get(j, "isOptional", v.is_optional);
}

struct MobileRobotAction {
    std::string action_type;
    std::optional<std::string> action_description;
    std::vector<ActionScope> action_scopes;
    std::optional<std::vector<FactsheetActionParameter>> action_parameters;  // нет — у action нет параметров
    std::optional<std::string> action_result;
    std::optional<std::vector<BlockingType>> blocking_types;
    bool pause_allowed = false;   // можно поставить на паузу startPause
    bool cancel_allowed = false;  // можно отменить cancelOrder
};

inline void to_json(json& j, const MobileRobotAction& v) {
    j = json::object();
    detail::put(j, "actionType", v.action_type);
    detail::put(j, "actionDescription", v.action_description);
    detail::put(j, "actionScopes", v.action_scopes);
    detail::put(j, "actionParameters", v.action_parameters);
    detail::put(j, "actionResult", v.action_result);
    detail::put(j, "blockingTypes", v.blocking_types);
    detail::put(j, "pauseAllowed", v.pause_allowed);
    detail::put(j, "cancelAllowed", v.cancel_allowed);
}

inline void from_json(const json& j, MobileRobotAction& v) {
    detail::get(j, "actionType", v.action_type);
    detail::get(j, "actionDescription", v.action_description);
    detail::get(j, "actionScopes", v.action_scopes);
    detail::get(j, "actionParameters", v.action_parameters);
    detail::get(j, "actionResult", v.action_result);
    detail::get(j, "blockingTypes", v.blocking_types);
    detail::get(j, "pauseAllowed", v.pause_allowed);
    detail::get(j, "cancelAllowed", v.cancel_allowed);
}

struct ProtocolFeatures {
    std::vector<OptionalParameter> optional_parameters;  // чего нет в списке — не поддерживается
    std::vector<MobileRobotAction> mobile_robot_actions;
};

inline void to_json(json& j, const ProtocolFeatures& v) {
    j = json::object();
    detail::put(j, "optionalParameters", v.optional_parameters);
    detail::put(j, "mobileRobotActions", v.mobile_robot_actions);
}

inline void from_json(const json& j, ProtocolFeatures& v) {
    detail::get(j, "optionalParameters", v.optional_parameters);
    detail::get(j, "mobileRobotActions", v.mobile_robot_actions);
}

// ============================================================================
// mobileRobotGeometry
// ============================================================================

namespace wheel_type {
inline constexpr const char* kDrive = "DRIVE";
inline constexpr const char* kCaster = "CASTER";
inline constexpr const char* kFixed = "FIXED";
inline constexpr const char* kMecanum = "MECANUM";
}  // namespace wheel_type

struct WheelPosition {
    double x = 0.0;                // [m] в системе координат робота
    double y = 0.0;                // [m]
    std::optional<double> theta;   // [rad], нужно для fixed-колёс
};

inline void to_json(json& j, const WheelPosition& v) {
    j = json::object();
    detail::put(j, "x", v.x);
    detail::put(j, "y", v.y);
    detail::put(j, "theta", v.theta);
}

inline void from_json(const json& j, WheelPosition& v) {
    detail::get(j, "x", v.x);
    detail::get(j, "y", v.y);
    detail::get(j, "theta", v.theta);
}

struct WheelDefinition {
    std::string type;  // см. wheel_type::*
    bool is_active_driven = false;
    bool is_active_steered = false;
    WheelPosition position;
    double diameter = 0.0;                        // [m]
    double width = 0.0;                           // [m]
    std::optional<double> center_displacement;    // [m], для caster; нет — 0
    std::optional<std::string> constraints;
};

inline void to_json(json& j, const WheelDefinition& v) {
    j = json::object();
    detail::put(j, "type", v.type);
    detail::put(j, "isActiveDriven", v.is_active_driven);
    detail::put(j, "isActiveSteered", v.is_active_steered);
    detail::put(j, "position", v.position);
    detail::put(j, "diameter", v.diameter);
    detail::put(j, "width", v.width);
    detail::put(j, "centerDisplacement", v.center_displacement);
    detail::put(j, "constraints", v.constraints);
}

inline void from_json(const json& j, WheelDefinition& v) {
    detail::get(j, "type", v.type);
    detail::get(j, "isActiveDriven", v.is_active_driven);
    detail::get(j, "isActiveSteered", v.is_active_steered);
    detail::get(j, "position", v.position);
    detail::get(j, "diameter", v.diameter);
    detail::get(j, "width", v.width);
    detail::get(j, "centerDisplacement", v.center_displacement);
    detail::get(j, "constraints", v.constraints);
}

// 2D-огибающая: механический контур с грузом/без, поля безопасности и т.п.
struct Envelope2d {
    std::string envelope2d_id;
    std::vector<Vertex> vertices;  // замкнутый простой полигон, [m]
    std::optional<std::string> description;
};

inline void to_json(json& j, const Envelope2d& v) {
    j = json::object();
    detail::put(j, "envelope2dId", v.envelope2d_id);
    detail::put(j, "vertices", v.vertices);
    detail::put(j, "description", v.description);
}

inline void from_json(const json& j, Envelope2d& v) {
    detail::get(j, "envelope2dId", v.envelope2d_id);
    detail::get(j, "vertices", v.vertices);
    detail::get(j, "description", v.description);
}

struct Envelope3d {
    std::string envelope3d_id;
    std::string format;               // напр. "DXF"
    std::optional<json> data;         // данные в формате format
    std::optional<std::string> url;   // или ссылка на скачивание
    std::optional<std::string> description;
};

inline void to_json(json& j, const Envelope3d& v) {
    j = json::object();
    detail::put(j, "envelope3dId", v.envelope3d_id);
    detail::put(j, "format", v.format);
    detail::put(j, "data", v.data);
    detail::put(j, "url", v.url);
    detail::put(j, "description", v.description);
}

inline void from_json(const json& j, Envelope3d& v) {
    detail::get(j, "envelope3dId", v.envelope3d_id);
    detail::get(j, "format", v.format);
    detail::get(j, "data", v.data);
    detail::get(j, "url", v.url);
    detail::get(j, "description", v.description);
}

struct MobileRobotGeometry {
    std::optional<std::vector<WheelDefinition>> wheel_definitions;
    std::optional<std::vector<Envelope2d>> envelopes2d;
    std::optional<std::vector<Envelope3d>> envelopes3d;
};

inline void to_json(json& j, const MobileRobotGeometry& v) {
    j = json::object();
    detail::put(j, "wheelDefinitions", v.wheel_definitions);
    detail::put(j, "envelopes2d", v.envelopes2d);
    detail::put(j, "envelopes3d", v.envelopes3d);
}

inline void from_json(const json& j, MobileRobotGeometry& v) {
    detail::get(j, "wheelDefinitions", v.wheel_definitions);
    detail::get(j, "envelopes2d", v.envelopes2d);
    detail::get(j, "envelopes3d", v.envelopes3d);
}

// ============================================================================
// loadSpecification
// ============================================================================

struct LoadSet {
    std::string set_name;   // "DEFAULT", "SET1", ...
    std::string load_type;  // "EPAL", "XLT1200", ...
    std::optional<std::vector<std::string>> load_positions;  // нет — для всех позиций
    std::optional<BoundingBoxReference> bounding_box_reference;
    std::optional<LoadDimensions> load_dimensions;
    std::optional<double> maximum_weight;                 // [kg]
    std::optional<double> minimum_loadhandling_height;    // [m]
    std::optional<double> maximum_loadhandling_height;    // [m]
    std::optional<double> minimum_loadhandling_depth;     // [m]
    std::optional<double> maximum_loadhandling_depth;     // [m]
    std::optional<double> minimum_loadhandling_tilt;      // [rad]
    std::optional<double> maximum_loadhandling_tilt;      // [rad]
    std::optional<double> maximum_speed;                  // [m/s]
    std::optional<double> maximum_acceleration;           // [m/s^2]
    std::optional<double> maximum_deceleration;           // [m/s^2]
    std::optional<double> pick_time;                      // [s]
    std::optional<double> drop_time;                      // [s]
    std::optional<std::string> description;
};

inline void to_json(json& j, const LoadSet& v) {
    j = json::object();
    detail::put(j, "setName", v.set_name);
    detail::put(j, "loadType", v.load_type);
    detail::put(j, "loadPositions", v.load_positions);
    detail::put(j, "boundingBoxReference", v.bounding_box_reference);
    detail::put(j, "loadDimensions", v.load_dimensions);
    detail::put(j, "maximumWeight", v.maximum_weight);
    detail::put(j, "minimumLoadhandlingHeight", v.minimum_loadhandling_height);
    detail::put(j, "maximumLoadhandlingHeight", v.maximum_loadhandling_height);
    detail::put(j, "minimumLoadhandlingDepth", v.minimum_loadhandling_depth);
    detail::put(j, "maximumLoadhandlingDepth", v.maximum_loadhandling_depth);
    detail::put(j, "minimumLoadhandlingTilt", v.minimum_loadhandling_tilt);
    detail::put(j, "maximumLoadhandlingTilt", v.maximum_loadhandling_tilt);
    detail::put(j, "maximumSpeed", v.maximum_speed);
    detail::put(j, "maximumAcceleration", v.maximum_acceleration);
    detail::put(j, "maximumDeceleration", v.maximum_deceleration);
    detail::put(j, "pickTime", v.pick_time);
    detail::put(j, "dropTime", v.drop_time);
    detail::put(j, "description", v.description);
}

inline void from_json(const json& j, LoadSet& v) {
    detail::get(j, "setName", v.set_name);
    detail::get(j, "loadType", v.load_type);
    detail::get(j, "loadPositions", v.load_positions);
    detail::get(j, "boundingBoxReference", v.bounding_box_reference);
    detail::get(j, "loadDimensions", v.load_dimensions);
    detail::get(j, "maximumWeight", v.maximum_weight);
    detail::get(j, "minimumLoadhandlingHeight", v.minimum_loadhandling_height);
    detail::get(j, "maximumLoadhandlingHeight", v.maximum_loadhandling_height);
    detail::get(j, "minimumLoadhandlingDepth", v.minimum_loadhandling_depth);
    detail::get(j, "maximumLoadhandlingDepth", v.maximum_loadhandling_depth);
    detail::get(j, "minimumLoadhandlingTilt", v.minimum_loadhandling_tilt);
    detail::get(j, "maximumLoadhandlingTilt", v.maximum_loadhandling_tilt);
    detail::get(j, "maximumSpeed", v.maximum_speed);
    detail::get(j, "maximumAcceleration", v.maximum_acceleration);
    detail::get(j, "maximumDeceleration", v.maximum_deceleration);
    detail::get(j, "pickTime", v.pick_time);
    detail::get(j, "dropTime", v.drop_time);
    detail::get(j, "description", v.description);
}

struct LoadSpecification {
    // Допустимые значения state.loads[].loadPosition и параметра lhd у pick/drop.
    // Нет или пусто — у робота нет грузозахвата.
    std::optional<std::vector<std::string>> load_positions;
    std::optional<std::vector<LoadSet>> load_sets;
};

inline void to_json(json& j, const LoadSpecification& v) {
    j = json::object();
    detail::put(j, "loadPositions", v.load_positions);
    detail::put(j, "loadSets", v.load_sets);
}

inline void from_json(const json& j, LoadSpecification& v) {
    detail::get(j, "loadPositions", v.load_positions);
    detail::get(j, "loadSets", v.load_sets);
}

// ============================================================================
// mobileRobotConfiguration
// ============================================================================

struct VersionInfo {
    std::string key;    // напр. "softwareVersion"
    std::string value;  // напр. "v1.12.4-beta"
};

inline void to_json(json& j, const VersionInfo& v) {
    j = json::object();
    detail::put(j, "key", v.key);
    detail::put(j, "value", v.value);
}

inline void from_json(const json& j, VersionInfo& v) {
    detail::get(j, "key", v.key);
    detail::get(j, "value", v.value);
}

// Не меняется, пока робот работает.
struct Network {
    std::optional<std::vector<std::string>> dns_servers;
    std::optional<std::vector<std::string>> ntp_servers;
    std::optional<std::string> local_ip_address;
    std::optional<std::string> netmask;
    std::optional<std::string> default_gateway;
};

inline void to_json(json& j, const Network& v) {
    j = json::object();
    detail::put(j, "dnsServers", v.dns_servers);
    detail::put(j, "ntpServers", v.ntp_servers);
    detail::put(j, "localIpAddress", v.local_ip_address);
    detail::put(j, "netmask", v.netmask);
    detail::put(j, "defaultGateway", v.default_gateway);
}

inline void from_json(const json& j, Network& v) {
    detail::get(j, "dnsServers", v.dns_servers);
    detail::get(j, "ntpServers", v.ntp_servers);
    detail::get(j, "localIpAddress", v.local_ip_address);
    detail::get(j, "netmask", v.netmask);
    detail::get(j, "defaultGateway", v.default_gateway);
}

struct BatteryCharging {
    std::optional<double> critical_low_charging_level;   // [%] ниже — только order на зарядку
    std::optional<double> maximum_desired_charging_level;  // [%]
    std::optional<double> minimum_desired_charging_level;  // [%]
    std::optional<std::uint32_t> minimum_charging_time;    // [s]
};

inline void to_json(json& j, const BatteryCharging& v) {
    j = json::object();
    detail::put(j, "criticalLowChargingLevel", v.critical_low_charging_level);
    detail::put(j, "maximumDesiredChargingLevel", v.maximum_desired_charging_level);
    detail::put(j, "minimumDesiredChargingLevel", v.minimum_desired_charging_level);
    detail::put(j, "minimumChargingTime", v.minimum_charging_time);
}

inline void from_json(const json& j, BatteryCharging& v) {
    detail::get(j, "criticalLowChargingLevel", v.critical_low_charging_level);
    detail::get(j, "maximumDesiredChargingLevel", v.maximum_desired_charging_level);
    detail::get(j, "minimumDesiredChargingLevel", v.minimum_desired_charging_level);
    detail::get(j, "minimumChargingTime", v.minimum_charging_time);
}

// В таблице 7.10 разметка поехала; по смыслу batteryCharging лежит здесь же,
// рядом с versions и network.
struct MobileRobotConfiguration {
    std::optional<std::vector<VersionInfo>> versions;
    std::optional<Network> network;
    std::optional<BatteryCharging> battery_charging;
};

inline void to_json(json& j, const MobileRobotConfiguration& v) {
    j = json::object();
    detail::put(j, "versions", v.versions);
    detail::put(j, "network", v.network);
    detail::put(j, "batteryCharging", v.battery_charging);
}

inline void from_json(const json& j, MobileRobotConfiguration& v) {
    detail::get(j, "versions", v.versions);
    detail::get(j, "network", v.network);
    detail::get(j, "batteryCharging", v.battery_charging);
}

// ============================================================================
// Сообщение factsheet
// ============================================================================

struct Factsheet {
    Header header;
    TypeSpecification type_specification;
    PhysicalParameters physical_parameters;
    ProtocolLimits protocol_limits;
    ProtocolFeatures protocol_features;
    MobileRobotGeometry mobile_robot_geometry;
    LoadSpecification load_specification;
    std::optional<MobileRobotConfiguration> mobile_robot_configuration;
};

inline void to_json(json& j, const Factsheet& v) {
    j = json::object();
    put_header(j, v.header);
    detail::put(j, "typeSpecification", v.type_specification);
    detail::put(j, "physicalParameters", v.physical_parameters);
    detail::put(j, "protocolLimits", v.protocol_limits);
    detail::put(j, "protocolFeatures", v.protocol_features);
    detail::put(j, "mobileRobotGeometry", v.mobile_robot_geometry);
    detail::put(j, "loadSpecification", v.load_specification);
    detail::put(j, "mobileRobotConfiguration", v.mobile_robot_configuration);
}

inline void from_json(const json& j, Factsheet& v) {
    get_header(j, v.header);
    detail::get(j, "typeSpecification", v.type_specification);
    detail::get(j, "physicalParameters", v.physical_parameters);
    detail::get(j, "protocolLimits", v.protocol_limits);
    detail::get(j, "protocolFeatures", v.protocol_features);
    detail::get(j, "mobileRobotGeometry", v.mobile_robot_geometry);
    detail::get(j, "loadSpecification", v.load_specification);
    detail::get(j, "mobileRobotConfiguration", v.mobile_robot_configuration);
}

}  // namespace vda5050
