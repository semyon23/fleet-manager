#pragma once

// VDA 5050 v3.0.0 — топик `state` (6.6, 7.8).
// Публикует: робот. Подписан: fleet control. QoS 0.
//
// Шлётся при событиях (новый order, смена errors/operatingMode/driving/paused/loads,
// проход узла, изменение actionStates и т.д.) и не реже раза в 30 с.
//
// Робот idle (6.6.8), если nodeStates и edgeStates пусты и все actionStates
// в FINISHED/FAILED. Новый order принимается только в idle.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "types.hpp"

namespace vda5050 {

// ============================================================================
// Карты и наборы зон на роботе (6.3.2)
// ============================================================================

enum class EnableStatus { Enabled, Disabled };
VDA5050_JSON_ENUM(EnableStatus, {
    {EnableStatus::Enabled, "ENABLED"},
    {EnableStatus::Disabled, "DISABLED"},
})

struct Map {
    std::string map_id;
    std::string map_version;
    EnableStatus map_status = EnableStatus::Disabled;  // на один mapId включена максимум одна версия
    std::optional<std::string> map_descriptor;
};

inline auto eq_key(const Map& v) {
    return std::tie(v.map_id, v.map_version, v.map_status, v.map_descriptor);
}

inline void to_json(json& j, const Map& v) {
    j = json::object();
    detail::put(j, "mapId", v.map_id);
    detail::put(j, "mapVersion", v.map_version);
    detail::put(j, "mapStatus", v.map_status);
    detail::put(j, "mapDescriptor", v.map_descriptor);
}

inline void from_json(const json& j, Map& v) {
    detail::get(j, "mapId", v.map_id);
    detail::get(j, "mapVersion", v.map_version);
    detail::get(j, "mapStatus", v.map_status);
    detail::get(j, "mapDescriptor", v.map_descriptor);
}

// Набор зон, загруженный на робота (не путать с ZoneSet из zone_set.hpp — там сами зоны).
struct ZoneSetInfo {
    std::string zone_set_id;
    std::string map_id;
    EnableStatus zone_set_status = EnableStatus::Disabled;  // на одну карту включён максимум один набор
};

inline auto eq_key(const ZoneSetInfo& v) {
    return std::tie(v.zone_set_id, v.map_id, v.zone_set_status);
}

inline void to_json(json& j, const ZoneSetInfo& v) {
    j = json::object();
    detail::put(j, "zoneSetId", v.zone_set_id);
    detail::put(j, "mapId", v.map_id);
    detail::put(j, "zoneSetStatus", v.zone_set_status);
}

inline void from_json(const json& j, ZoneSetInfo& v) {
    detail::get(j, "zoneSetId", v.zone_set_id);
    detail::get(j, "mapId", v.map_id);
    detail::get(j, "zoneSetStatus", v.zone_set_status);
}

// ============================================================================
// Прогресс order (6.6.1, 6.6.2)
// ============================================================================

// Узел, который ещё предстоит пройти. Пройденный узел робот удаляет из nodeStates
// и пишет его в lastNodeId / lastNodeSequenceId.
struct NodeState {
    std::string node_id;
    std::uint32_t sequence_id = 0;
    std::optional<std::string> node_descriptor;
    bool released = false;
    std::optional<NodePosition> node_position;  // для отладки, fleet control и так знает
};

inline auto eq_key(const NodeState& v) {
    return std::tie(v.node_id, v.sequence_id, v.node_descriptor, v.released, v.node_position);
}

inline void to_json(json& j, const NodeState& v) {
    j = json::object();
    detail::put(j, "nodeId", v.node_id);
    detail::put(j, "sequenceId", v.sequence_id);
    detail::put(j, "nodeDescriptor", v.node_descriptor);
    detail::put(j, "released", v.released);
    detail::put(j, "nodePosition", v.node_position);
}

inline void from_json(const json& j, NodeState& v) {
    detail::get(j, "nodeId", v.node_id);
    detail::get(j, "sequenceId", v.sequence_id);
    detail::get(j, "nodeDescriptor", v.node_descriptor);
    detail::get(j, "released", v.released);
    detail::get(j, "nodePosition", v.node_position);
}

struct EdgeState {
    std::string edge_id;
    std::uint32_t sequence_id = 0;
    std::optional<std::string> edge_descriptor;
    bool released = false;
    std::optional<Trajectory> trajectory;  // только траектория из layout/order, не спланированная роботом
};

inline auto eq_key(const EdgeState& v) {
    return std::tie(v.edge_id, v.sequence_id, v.edge_descriptor, v.released, v.trajectory);
}

inline void to_json(json& j, const EdgeState& v) {
    j = json::object();
    detail::put(j, "edgeId", v.edge_id);
    detail::put(j, "sequenceId", v.sequence_id);
    detail::put(j, "edgeDescriptor", v.edge_descriptor);
    detail::put(j, "released", v.released);
    detail::put(j, "trajectory", v.trajectory);
}

inline void from_json(const json& j, EdgeState& v) {
    detail::get(j, "edgeId", v.edge_id);
    detail::get(j, "sequenceId", v.sequence_id);
    detail::get(j, "edgeDescriptor", v.edge_descriptor);
    detail::get(j, "released", v.released);
    detail::get(j, "trajectory", v.trajectory);
}

// ============================================================================
// Груз
// ============================================================================

struct Load {
    std::optional<std::string> load_id;        // штрихкод / RFID; "" — умеет читать, но ещё не прочитал
    std::optional<std::string> load_type;
    std::optional<std::string> load_position;  // "front", "back", ... (значения из factsheet.loadSpecification)
    std::optional<BoundingBoxReference> bounding_box_reference;
    std::optional<LoadDimensions> load_dimensions;
    std::optional<double> weight;              // [kg]
};

inline auto eq_key(const Load& v) {
    return std::tie(v.load_id, v.load_type, v.load_position, v.bounding_box_reference, v.load_dimensions, v.weight);
}

inline void to_json(json& j, const Load& v) {
    j = json::object();
    detail::put(j, "loadId", v.load_id);
    detail::put(j, "loadType", v.load_type);
    detail::put(j, "loadPosition", v.load_position);
    detail::put(j, "boundingBoxReference", v.bounding_box_reference);
    detail::put(j, "loadDimensions", v.load_dimensions);
    detail::put(j, "weight", v.weight);
}

inline void from_json(const json& j, Load& v) {
    detail::get(j, "loadId", v.load_id);
    detail::get(j, "loadType", v.load_type);
    detail::get(j, "loadPosition", v.load_position);
    detail::get(j, "boundingBoxReference", v.bounding_box_reference);
    detail::get(j, "loadDimensions", v.load_dimensions);
    detail::get(j, "weight", v.weight);
}

// ============================================================================
// Запросы робота к fleet control (6.9). Ответ — топик `responses`.
// ============================================================================

// 7.8 перечисляет REQUESTED/GRANTED/REVOKED/EXPIRED, а 6.9 добавляет QUEUED.
// Принимаем все пять, чтобы не падать на роботах, которые шлют QUEUED.
enum class RequestStatus { Requested, Granted, Revoked, Expired, Queued };
VDA5050_JSON_ENUM(RequestStatus, {
    {RequestStatus::Requested, "REQUESTED"},
    {RequestStatus::Granted, "GRANTED"},
    {RequestStatus::Revoked, "REVOKED"},
    {RequestStatus::Expired, "EXPIRED"},
    {RequestStatus::Queued, "QUEUED"},
})

enum class ZoneRequestType {
    Access,      // вход в RELEASE-зону
    Replanning,  // путь через COORDINATED_REPLANNING-зону
};
VDA5050_JSON_ENUM(ZoneRequestType, {
    {ZoneRequestType::Access, "ACCESS"},
    {ZoneRequestType::Replanning, "REPLANNING"},
})

struct ZoneRequest {
    std::string request_id;  // уникален среди всех активных запросов робота (zone + edge)
    ZoneRequestType request_type = ZoneRequestType::Access;
    std::string zone_id;
    std::string zone_set_id;
    RequestStatus request_status = RequestStatus::Requested;
    std::optional<Trajectory> trajectory;  // только для REPLANNING
};

inline auto eq_key(const ZoneRequest& v) {
    return std::tie(v.request_id, v.request_type, v.zone_id, v.zone_set_id, v.request_status, v.trajectory);
}

inline void to_json(json& j, const ZoneRequest& v) {
    j = json::object();
    detail::put(j, "requestId", v.request_id);
    detail::put(j, "requestType", v.request_type);
    detail::put(j, "zoneId", v.zone_id);
    detail::put(j, "zoneSetId", v.zone_set_id);
    detail::put(j, "requestStatus", v.request_status);
    detail::put(j, "trajectory", v.trajectory);
}

inline void from_json(const json& j, ZoneRequest& v) {
    detail::get(j, "requestId", v.request_id);
    detail::get(j, "requestType", v.request_type);
    detail::get(j, "zoneId", v.zone_id);
    detail::get(j, "zoneSetId", v.zone_set_id);
    detail::get(j, "requestStatus", v.request_status);
    detail::get(j, "trajectory", v.trajectory);
}

enum class EdgeRequestType { Corridor };
VDA5050_JSON_ENUM(EdgeRequestType, {
    {EdgeRequestType::Corridor, "CORRIDOR"},
})

// Запрос на использование коридора ребра (6.6.10), если corridor.releaseRequired = true.
struct EdgeRequest {
    std::string request_id;
    EdgeRequestType request_type = EdgeRequestType::Corridor;
    std::string edge_id;
    std::uint32_t sequence_id = 0;
    RequestStatus request_status = RequestStatus::Requested;
};

inline auto eq_key(const EdgeRequest& v) {
    return std::tie(v.request_id, v.request_type, v.edge_id, v.sequence_id, v.request_status);
}

inline void to_json(json& j, const EdgeRequest& v) {
    j = json::object();
    detail::put(j, "requestId", v.request_id);
    detail::put(j, "requestType", v.request_type);
    detail::put(j, "edgeId", v.edge_id);
    detail::put(j, "sequenceId", v.sequence_id);
    detail::put(j, "requestStatus", v.request_status);
}

inline void from_json(const json& j, EdgeRequest& v) {
    detail::get(j, "requestId", v.request_id);
    detail::get(j, "requestType", v.request_type);
    detail::get(j, "edgeId", v.edge_id);
    detail::get(j, "sequenceId", v.sequence_id);
    detail::get(j, "requestStatus", v.request_status);
}

// ============================================================================
// Статусы action (6.6.9)
// ============================================================================

enum class ActionStatus {
    Waiting,       // получена, узел ещё не пройден / ребро не начато
    Initializing,  // подготовка
    Running,
    Paused,
    Retriable,     // упала, можно повторить (instant action retry / skipRetry)
    Finished,
    Failed,
};
VDA5050_JSON_ENUM(ActionStatus, {
    {ActionStatus::Waiting, "WAITING"},
    {ActionStatus::Initializing, "INITIALIZING"},
    {ActionStatus::Running, "RUNNING"},
    {ActionStatus::Paused, "PAUSED"},
    {ActionStatus::Retriable, "RETRIABLE"},
    {ActionStatus::Finished, "FINISHED"},
    {ActionStatus::Failed, "FAILED"},
})

struct ActionState {
    std::string action_id;
    std::optional<std::string> action_type;        // для информации
    std::optional<std::string> action_descriptor;
    ActionStatus action_status = ActionStatus::Waiting;
    std::optional<std::string> action_result;      // напр. результат чтения RFID
};

inline auto eq_key(const ActionState& v) {
    return std::tie(v.action_id, v.action_type, v.action_descriptor, v.action_status, v.action_result);
}

inline void to_json(json& j, const ActionState& v) {
    j = json::object();
    detail::put(j, "actionId", v.action_id);
    detail::put(j, "actionType", v.action_type);
    detail::put(j, "actionDescriptor", v.action_descriptor);
    detail::put(j, "actionStatus", v.action_status);
    detail::put(j, "actionResult", v.action_result);
}

inline void from_json(const json& j, ActionState& v) {
    detail::get(j, "actionId", v.action_id);
    detail::get(j, "actionType", v.action_type);
    detail::get(j, "actionDescriptor", v.action_descriptor);
    detail::get(j, "actionStatus", v.action_status);
    detail::get(j, "actionResult", v.action_result);
}

// ============================================================================
// Питание
// ============================================================================

struct PowerSupply {
    double state_of_charge = 0.0;                // [%] 0..100; для постоянного питания — 100
    std::optional<double> battery_voltage;       // [V]
    std::optional<double> battery_current;       // [A]
    std::optional<std::int8_t> battery_health;   // [%] 0..100
    bool charging = false;                       // false только если робот готов брать order
    std::optional<std::uint32_t> range;          // [m] оценка запаса хода
};

inline void to_json(json& j, const PowerSupply& v) {
    j = json::object();
    detail::put(j, "stateOfCharge", v.state_of_charge);
    detail::put(j, "batteryVoltage", v.battery_voltage);
    detail::put(j, "batteryCurrent", v.battery_current);
    detail::put(j, "batteryHealth", v.battery_health);
    detail::put(j, "charging", v.charging);
    detail::put(j, "range", v.range);
}

inline void from_json(const json& j, PowerSupply& v) {
    detail::get(j, "stateOfCharge", v.state_of_charge);
    detail::get(j, "batteryVoltage", v.battery_voltage);
    detail::get(j, "batteryCurrent", v.battery_current);
    detail::get(j, "batteryHealth", v.battery_health);
    detail::get(j, "charging", v.charging);
    detail::get(j, "range", v.range);
}

// ============================================================================
// Режим работы (6.6.6)
// ============================================================================

//                  fleet control  order при входе  instantActions      orders
// AUTOMATIC        управляет      сохраняется      можно               можно
// SEMIAUTOMATIC    управляет      сохраняется      можно               можно
// INTERVENED       нет            сохраняется      только cancelOrder  можно (выполнит после)
// MANUAL           нет            сбрасывается     нельзя              нельзя
// STARTUP          нет            сбрасывается     нельзя              нельзя (state может быть невалиден)
// SERVICE          нет            сбрасывается     нельзя              нельзя
// TEACH_IN         нет            сбрасывается     нельзя              нельзя (напр. построение карты)
enum class OperatingMode { Startup, Automatic, Semiautomatic, Intervened, Manual, Service, TeachIn };
VDA5050_JSON_ENUM(OperatingMode, {
    {OperatingMode::Startup, "STARTUP"},
    {OperatingMode::Automatic, "AUTOMATIC"},
    {OperatingMode::Semiautomatic, "SEMIAUTOMATIC"},
    {OperatingMode::Intervened, "INTERVENED"},
    {OperatingMode::Manual, "MANUAL"},
    {OperatingMode::Service, "SERVICE"},
    {OperatingMode::TeachIn, "TEACH_IN"},
})

// ============================================================================
// Ошибки и информация (6.6.4, 6.6.5)
// ============================================================================

// 'WARNING'  — внимания не требует, order продолжается, новые принимаются
// 'URGENT'   — нужно внимание (напр. низкий заряд), order продолжается, новые принимаются
// 'CRITICAL' — текущий order продолжить нельзя, новый принять можно
// 'FATAL'    — нужен человек, ни продолжить, ни принять новый нельзя
enum class ErrorLevel { Warning, Urgent, Critical, Fatal };
VDA5050_JSON_ENUM(ErrorLevel, {
    {ErrorLevel::Warning, "WARNING"},
    {ErrorLevel::Urgent, "URGENT"},
    {ErrorLevel::Critical, "CRITICAL"},
    {ErrorLevel::Fatal, "FATAL"},
})

// Предопределённые errorType (таблица 9 + упомянутые в 7.8).
// errorType — расширяемый enum, поэтому в Error это std::string.
namespace error_type {
inline constexpr const char* kUnsupportedParameter = "UNSUPPORTED_PARAMETER";          // CRITICAL
inline constexpr const char* kNoOrderToCancel = "NO_ORDER_TO_CANCEL";                  // WARNING
inline constexpr const char* kValidationFailure = "VALIDATION_FAILURE";                // WARNING
inline constexpr const char* kInvalidOrder = "INVALID_ORDER";                          // есть только в перечне 7.8
inline constexpr const char* kInvalidOrderAction = "INVALID_ORDER_ACTION";             // WARNING
inline constexpr const char* kInvalidInstantAction = "INVALID_INSTANT_ACTION";         // WARNING
inline constexpr const char* kOutdatedOrderUpdate = "OUTDATED_ORDER_UPDATE";           // WARNING
inline constexpr const char* kSameOrderUpdateId = "SAME_ORDER_UPDATE_ID";              // WARNING
inline constexpr const char* kOrderUpdateFollowingCancel = "ORDER_UPDATE_FOLLOWING_CANCEL";  // WARNING
inline constexpr const char* kOutsideOfCorridor = "OUTSIDE_OF_CORRIDOR";               // CRITICAL
inline constexpr const char* kInsufficientMemory = "INSUFFICIENT_MEMORY";              // URGENT
inline constexpr const char* kDuplicateMap = "DUPLICATE_MAP";                          // WARNING
inline constexpr const char* kBlockedZoneViolation = "BLOCKED_ZONE_VIOLATION";         // CRITICAL
inline constexpr const char* kDuplicateZoneSet = "DUPLICATE_ZONE_SET";                 // WARNING
inline constexpr const char* kReleaseLost = "RELEASE_LOST";                            // CRITICAL
inline constexpr const char* kZoneActionConflict = "ZONE_ACTION_CONFLICT";             // CRITICAL
inline constexpr const char* kNodeUnreachable = "NODE_UNREACHABLE";                    // CRITICAL
inline constexpr const char* kLocalizationError = "LOCALIZATION_ERROR";                // FATAL
inline constexpr const char* kNoRouteToTarget = "NO_ROUTE_TO_TARGET";                  // WARNING
inline constexpr const char* kOtherOrderActive = "OTHER_ORDER_ACTIVE";                 // WARNING
inline constexpr const char* kStartNodeOutOfRange = "START_NODE_OUT_OF_RANGE";         // WARNING
inline constexpr const char* kMobileRobotNotAvailable = "MOBILE_ROBOT_NOT_AVAILABLE";  // WARNING
inline constexpr const char* kUnknownMapId = "UNKNOWN_MAP_ID";                         // WARNING
inline constexpr const char* kInstantActionStatesFull = "INSTANT_ACTION_STATES_FULL";  // URGENT
inline constexpr const char* kZoneActionStatesFull = "ZONE_ACTION_STATES_FULL";        // URGENT
}  // namespace error_type

// Ссылка key/value — одинаковая структура у errorReference и infoReference.
struct Reference {
    std::string reference_key;    // "nodeId", "edgeId", "orderId", "actionId", "headerId", ...
    std::string reference_value;
};

inline auto eq_key(const Reference& v) {
    return std::tie(v.reference_key, v.reference_value);
}
using ErrorReference = Reference;
using InfoReference = Reference;

inline void to_json(json& j, const Reference& v) {
    j = json::object();
    detail::put(j, "referenceKey", v.reference_key);
    detail::put(j, "referenceValue", v.reference_value);
}

inline void from_json(const json& j, Reference& v) {
    detail::get(j, "referenceKey", v.reference_key);
    detail::get(j, "referenceValue", v.reference_value);
}

struct Translation {
    std::string translation_key;    // код языка ISO 639-1: "en", "ru", ...
    std::string translation_value;
};

inline auto eq_key(const Translation& v) {
    return std::tie(v.translation_key, v.translation_value);
}

inline void to_json(json& j, const Translation& v) {
    j = json::object();
    detail::put(j, "translationKey", v.translation_key);
    detail::put(j, "translationValue", v.translation_value);
}

inline void from_json(const json& j, Translation& v) {
    detail::get(j, "translationKey", v.translation_key);
    detail::get(j, "translationValue", v.translation_value);
}

struct Error {
    std::string error_type;  // см. error_type::*
    std::optional<std::vector<ErrorReference>> error_references;
    std::optional<std::string> error_description;
    std::optional<std::vector<Translation>> error_description_translations;
    std::optional<std::string> error_hint;
    std::optional<std::vector<Translation>> error_hint_translations;
    ErrorLevel error_level = ErrorLevel::Warning;
};

inline auto eq_key(const Error& v) {
    return std::tie(v.error_type,
                    v.error_references,
                    v.error_description,
                    v.error_description_translations,
                    v.error_hint,
                    v.error_hint_translations,
                    v.error_level);
}

inline void to_json(json& j, const Error& v) {
    j = json::object();
    detail::put(j, "errorType", v.error_type);
    detail::put(j, "errorReferences", v.error_references);
    detail::put(j, "errorDescription", v.error_description);
    detail::put(j, "errorDescriptionTranslations", v.error_description_translations);
    detail::put(j, "errorHint", v.error_hint);
    detail::put(j, "errorHintTranslations", v.error_hint_translations);
    detail::put(j, "errorLevel", v.error_level);
}

inline void from_json(const json& j, Error& v) {
    detail::get(j, "errorType", v.error_type);
    detail::get(j, "errorReferences", v.error_references);
    detail::get(j, "errorDescription", v.error_description);
    detail::get(j, "errorDescriptionTranslations", v.error_description_translations);
    detail::get(j, "errorHint", v.error_hint);
    detail::get(j, "errorHintTranslations", v.error_hint_translations);
    detail::get(j, "errorLevel", v.error_level);
}

enum class InfoLevel { Debug, Info };
VDA5050_JSON_ENUM(InfoLevel, {
    {InfoLevel::Debug, "DEBUG"},
    {InfoLevel::Info, "INFO"},
})

// Информация только для визуализации/отладки — в логике fleet control не использовать.
struct Info {
    std::string info_type;
    std::optional<std::vector<InfoReference>> info_references;
    std::optional<std::string> info_descriptor;
    InfoLevel info_level = InfoLevel::Info;
};

inline void to_json(json& j, const Info& v) {
    j = json::object();
    detail::put(j, "infoType", v.info_type);
    detail::put(j, "infoReferences", v.info_references);
    detail::put(j, "infoDescriptor", v.info_descriptor);
    detail::put(j, "infoLevel", v.info_level);
}

inline void from_json(const json& j, Info& v) {
    detail::get(j, "infoType", v.info_type);
    detail::get(j, "infoReferences", v.info_references);
    detail::get(j, "infoDescriptor", v.info_descriptor);
    detail::get(j, "infoLevel", v.info_level);
}

// ============================================================================
// Безопасность
// ============================================================================

enum class EmergencyStop {
    Manual,  // квитировать вручную на роботе
    Remote,  // аварийный стоп площадки, квитируется удалённо
    None,
};
VDA5050_JSON_ENUM(EmergencyStop, {
    {EmergencyStop::Manual, "MANUAL"},
    {EmergencyStop::Remote, "REMOTE"},
    {EmergencyStop::None, "NONE"},
})

struct SafetyState {
    EmergencyStop active_emergency_stop = EmergencyStop::None;
    bool field_violation = false;  // нарушено защитное поле (лидар, бампер)
};

inline auto eq_key(const SafetyState& v) {
    return std::tie(v.active_emergency_stop, v.field_violation);
}

inline void to_json(json& j, const SafetyState& v) {
    j = json::object();
    detail::put(j, "activeEmergencyStop", v.active_emergency_stop);
    detail::put(j, "fieldViolation", v.field_violation);
}

inline void from_json(const json& j, SafetyState& v) {
    detail::get(j, "activeEmergencyStop", v.active_emergency_stop);
    detail::get(j, "fieldViolation", v.field_violation);
}

// ============================================================================
// Сообщение state
// ============================================================================

struct State {
    Header header;

    std::optional<std::vector<Map>> maps;
    std::optional<std::vector<ZoneSetInfo>> zone_sets;

    std::string order_id;                  // "" если order ещё не было; держится до нового order
    std::uint32_t order_update_id = 0;
    std::string last_node_id;              // "" если нет
    std::uint32_t last_node_sequence_id = 0;  // игнорировать, если last_node_id пустой

    std::vector<NodeState> node_states;    // пусто в idle
    std::vector<EdgeState> edge_states;    // пусто в idle
    std::optional<PlannedPath> planned_path;
    std::optional<IntermediatePath> intermediate_path;

    std::optional<MobileRobotPosition> mobile_robot_position;  // нет только у роботов без локализации
    std::optional<Velocity> velocity;

    // nullopt — робот не умеет определять груз; пустой вектор — умеет, груза нет.
    std::optional<std::vector<Load>> loads;

    bool driving = false;                  // только езда, без подъёма вил и т.п.
    std::optional<bool> paused;
    std::optional<bool> new_base_request;  // true — base заканчивается, пора слать update
    std::optional<std::vector<ZoneRequest>> zone_requests;
    std::optional<std::vector<EdgeRequest>> edge_requests;
    std::optional<double> distance_since_last_node;  // [m], line-guided

    std::vector<ActionState> action_states;
    std::vector<ActionState> instant_action_states;  // живут до clearInstantActions
    std::optional<std::vector<ActionState>> zone_action_states;  // обязательно, если робот умеет ACTION-зоны

    PowerSupply power_supply;
    OperatingMode operating_mode = OperatingMode::Startup;
    std::vector<Error> errors;             // пусто — ошибок нет
    std::optional<std::vector<Info>> information;
    SafetyState safety_state;
};

// Поля, изменение которых по 6.6 обязывает сразу отправить state.
// State == State сравнивает только их: header, позиция, скорость, заряд, пути,
// distanceSinceLastNode и information меняются постоянно / не являются событием —
// их отправляем по таймеру (не реже раза в 30 с).
//
//   if (cur != last_sent) publish(cur);
inline auto eq_key(const State& v) {
    return std::tie(v.order_id,                // новый order
                    v.order_update_id,         // order update
                    v.last_node_id,
                    v.last_node_sequence_id,
                    v.node_states,
                    v.edge_states,
                    v.loads,
                    v.driving,
                    v.paused,
                    v.new_base_request,
                    v.zone_requests,
                    v.edge_requests,
                    v.action_states,
                    v.instant_action_states,
                    v.zone_action_states,
                    v.power_supply.charging,
                    v.operating_mode,
                    v.errors,
                    v.safety_state,
                    v.zone_sets,
                    v.maps);
}

inline void to_json(json& j, const State& v) {
    j = json::object();
    put_header(j, v.header);
    detail::put(j, "maps", v.maps);
    detail::put(j, "zoneSets", v.zone_sets);
    detail::put(j, "orderId", v.order_id);
    detail::put(j, "orderUpdateId", v.order_update_id);
    detail::put(j, "lastNodeId", v.last_node_id);
    detail::put(j, "lastNodeSequenceId", v.last_node_sequence_id);
    detail::put(j, "nodeStates", v.node_states);
    detail::put(j, "edgeStates", v.edge_states);
    detail::put(j, "plannedPath", v.planned_path);
    detail::put(j, "intermediatePath", v.intermediate_path);
    detail::put(j, "mobileRobotPosition", v.mobile_robot_position);
    detail::put(j, "velocity", v.velocity);
    detail::put(j, "loads", v.loads);
    detail::put(j, "driving", v.driving);
    detail::put(j, "paused", v.paused);
    detail::put(j, "newBaseRequest", v.new_base_request);
    detail::put(j, "zoneRequests", v.zone_requests);
    detail::put(j, "edgeRequests", v.edge_requests);
    detail::put(j, "distanceSinceLastNode", v.distance_since_last_node);
    detail::put(j, "actionStates", v.action_states);
    detail::put(j, "instantActionStates", v.instant_action_states);
    detail::put(j, "zoneActionStates", v.zone_action_states);
    detail::put(j, "powerSupply", v.power_supply);
    detail::put(j, "operatingMode", v.operating_mode);
    detail::put(j, "errors", v.errors);
    detail::put(j, "information", v.information);
    detail::put(j, "safetyState", v.safety_state);
}

inline void from_json(const json& j, State& v) {
    get_header(j, v.header);
    detail::get(j, "maps", v.maps);
    detail::get(j, "zoneSets", v.zone_sets);
    detail::get(j, "orderId", v.order_id);
    detail::get(j, "orderUpdateId", v.order_update_id);
    detail::get(j, "lastNodeId", v.last_node_id);
    detail::get(j, "lastNodeSequenceId", v.last_node_sequence_id);
    detail::get(j, "nodeStates", v.node_states);
    detail::get(j, "edgeStates", v.edge_states);
    detail::get(j, "plannedPath", v.planned_path);
    detail::get(j, "intermediatePath", v.intermediate_path);
    detail::get(j, "mobileRobotPosition", v.mobile_robot_position);
    detail::get(j, "velocity", v.velocity);
    detail::get(j, "loads", v.loads);
    detail::get(j, "driving", v.driving);
    detail::get(j, "paused", v.paused);
    detail::get(j, "newBaseRequest", v.new_base_request);
    detail::get(j, "zoneRequests", v.zone_requests);
    detail::get(j, "edgeRequests", v.edge_requests);
    detail::get(j, "distanceSinceLastNode", v.distance_since_last_node);
    detail::get(j, "actionStates", v.action_states);
    detail::get(j, "instantActionStates", v.instant_action_states);
    detail::get(j, "zoneActionStates", v.zone_action_states);
    detail::get(j, "powerSupply", v.power_supply);
    detail::get(j, "operatingMode", v.operating_mode);
    detail::get(j, "errors", v.errors);
    detail::get(j, "information", v.information);
    detail::get(j, "safetyState", v.safety_state);
}

// Робот idle по 6.6.8.
inline bool is_idle(const State& s) {
    if (!s.node_states.empty() || !s.edge_states.empty()) {
        return false;
    }
    for (const auto& a : s.action_states) {
        if (a.action_status != ActionStatus::Finished && a.action_status != ActionStatus::Failed) {
            return false;
        }
    }
    return true;
}

}  // namespace vda5050
