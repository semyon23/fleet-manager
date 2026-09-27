#pragma once

// VDA 5050 v3.0.0 — топик `zoneSet` (6.4, 7.6). Опциональный.
// Публикует: fleet control. Подписан: робот. QoS 0.
//
// Набор зон для одной карты (mapId, без mapVersion). Содержимое zoneSetId неизменно:
// поменялись зоны — новый zoneSetId. Новый набор приходит DISABLED,
// включается instant action enableZoneSet. На одну карту активен один набор.
// Какие типы зон понимает робот — factsheet.typeSpecification.supportedZones.

#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "types.hpp"

namespace vda5050 {

// Что делает робот в RELEASE-зоне, если разрешение отозвано или истекло.
enum class ZoneReleaseLossBehavior { Stop, Continue, Evacuate };
VDA5050_JSON_ENUM(ZoneReleaseLossBehavior, {
    {ZoneReleaseLossBehavior::Stop, "STOP"},
    {ZoneReleaseLossBehavior::Continue, "CONTINUE"},
    {ZoneReleaseLossBehavior::Evacuate, "EVACUATE"},
})

enum class DirectedLimitation { Soft, Restricted, Strict };
VDA5050_JSON_ENUM(DirectedLimitation, {
    {DirectedLimitation::Soft, "SOFT"},
    {DirectedLimitation::Restricted, "RESTRICTED"},
    {DirectedLimitation::Strict, "STRICT"},
})

enum class BidirectedLimitation { Soft, Restricted };
VDA5050_JSON_ENUM(BidirectedLimitation, {
    {BidirectedLimitation::Soft, "SOFT"},
    {BidirectedLimitation::Restricted, "RESTRICTED"},
})

// Action внутри ACTION-зоны. Как Action, но без actionId: его генерирует сам робот.
struct ZoneAction {
    std::string action_type;
    std::optional<std::string> action_descriptor;
    BlockingType blocking_type = BlockingType::None;
    std::optional<std::vector<ActionParameter>> action_parameters;
    std::optional<bool> retriable;  // default false
};

inline void to_json(json& j, const ZoneAction& v) {
    j = json::object();
    detail::put(j, "actionType", v.action_type);
    detail::put(j, "actionDescriptor", v.action_descriptor);
    detail::put(j, "blockingType", v.blocking_type);
    detail::put(j, "actionParameters", v.action_parameters);
    detail::put(j, "retriable", v.retriable);
}

inline void from_json(const json& j, ZoneAction& v) {
    detail::get(j, "actionType", v.action_type);
    detail::get(j, "actionDescriptor", v.action_descriptor);
    detail::get(j, "blockingType", v.blocking_type);
    detail::get(j, "actionParameters", v.action_parameters);
    detail::get(j, "retriable", v.retriable);
}

// Зона. Какие optional поля обязательны — зависит от zone_type (см. комментарии).
struct Zone {
    std::string zone_id;                        // уникален внутри zoneSet
    ZoneType zone_type = ZoneType::Blocked;
    std::optional<std::string> zone_descriptor;
    std::vector<Vertex> vertices;               // простой полигон против часовой стрелки, >= 3 вершин

    std::optional<double> maximum_speed;                       // SPEED_LIMIT, [m/s]
    std::optional<std::vector<ZoneAction>> entry_actions;      // ACTION
    std::optional<std::vector<ZoneAction>> during_actions;     // ACTION
    std::optional<std::vector<ZoneAction>> exit_actions;       // ACTION
    std::optional<ZoneReleaseLossBehavior> release_loss_behavior;  // RELEASE, нет — STOP + ошибка
    std::optional<double> priority_factor;                     // PRIORITY, [0..1]
    std::optional<double> penalty_factor;                      // PENALTY, [0..1]
    std::optional<double> direction;                           // DIRECTED / BIDIRECTED, [rad]
    std::optional<DirectedLimitation> directed_limitation;     // DIRECTED
    std::optional<BidirectedLimitation> bidirected_limitation; // BIDIRECTED
};

inline void to_json(json& j, const Zone& v) {
    j = json::object();
    detail::put(j, "zoneId", v.zone_id);
    detail::put(j, "zoneType", v.zone_type);
    detail::put(j, "zoneDescriptor", v.zone_descriptor);
    detail::put(j, "vertices", v.vertices);
    detail::put(j, "maximumSpeed", v.maximum_speed);
    detail::put(j, "entryActions", v.entry_actions);
    detail::put(j, "duringActions", v.during_actions);
    detail::put(j, "exitActions", v.exit_actions);
    detail::put(j, "releaseLossBehavior", v.release_loss_behavior);
    detail::put(j, "priorityFactor", v.priority_factor);
    detail::put(j, "penaltyFactor", v.penalty_factor);
    detail::put(j, "direction", v.direction);
    detail::put(j, "directedLimitation", v.directed_limitation);
    detail::put(j, "bidirectedLimitation", v.bidirected_limitation);
}

inline void from_json(const json& j, Zone& v) {
    detail::get(j, "zoneId", v.zone_id);
    detail::get(j, "zoneType", v.zone_type);
    detail::get(j, "zoneDescriptor", v.zone_descriptor);
    detail::get(j, "vertices", v.vertices);
    detail::get(j, "maximumSpeed", v.maximum_speed);
    detail::get(j, "entryActions", v.entry_actions);
    detail::get(j, "duringActions", v.during_actions);
    detail::get(j, "exitActions", v.exit_actions);
    detail::get(j, "releaseLossBehavior", v.release_loss_behavior);
    detail::get(j, "priorityFactor", v.priority_factor);
    detail::get(j, "penaltyFactor", v.penalty_factor);
    detail::get(j, "direction", v.direction);
    detail::get(j, "directedLimitation", v.directed_limitation);
    detail::get(j, "bidirectedLimitation", v.bidirected_limitation);
}

struct ZoneSet {
    std::string map_id;
    std::string zone_set_id;                        // глобально уникален
    std::optional<std::string> zone_set_descriptor;
    std::vector<Zone> zones;
};

inline void to_json(json& j, const ZoneSet& v) {
    j = json::object();
    detail::put(j, "mapId", v.map_id);
    detail::put(j, "zoneSetId", v.zone_set_id);
    detail::put(j, "zoneSetDescriptor", v.zone_set_descriptor);
    detail::put(j, "zones", v.zones);
}

inline void from_json(const json& j, ZoneSet& v) {
    detail::get(j, "mapId", v.map_id);
    detail::get(j, "zoneSetId", v.zone_set_id);
    detail::get(j, "zoneSetDescriptor", v.zone_set_descriptor);
    detail::get(j, "zones", v.zones);
}

// Само сообщение топика `zoneSet`: заголовок + объект zoneSet.
struct ZoneSetMessage {
    Header header;
    ZoneSet zone_set;
};

inline void to_json(json& j, const ZoneSetMessage& v) {
    j = json::object();
    put_header(j, v.header);
    detail::put(j, "zoneSet", v.zone_set);
}

inline void from_json(const json& j, ZoneSetMessage& v) {
    get_header(j, v.header);
    detail::get(j, "zoneSet", v.zone_set);
}

}  // namespace vda5050
