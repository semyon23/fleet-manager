
#ifndef FLEETMANAGERSYSTEM_AVAILABILITY_HPP
#define FLEETMANAGERSYSTEM_AVAILABILITY_HPP
#pragma once

#include "fleet_database.pb.h"
#include "maps/Graph.hpp"
#include <cmath>

class MapStore;

inline constexpr double kOnNodeTolerance = 0.5; // metres

inline bool stopped_on_node(const fms::v1::RobotStatus& s, const Graph& graph, const std::string& node) {
    if (s.driving() || !s.localized() || !s.has_pose() || s.pose().map_id() != graph.map_id ||
        s.last_node_id() != node) return false;
    const auto index = graph.find_node(node);
    if (!index) return false;
    const auto& n = graph.nodes[*index];
    return std::hypot(s.pose().x() - n.x, s.pose().y() - n.y) <= kOnNodeTolerance;
}

// Может ли робот брать работу и почему нет — контракт UI, раздел 5.1. Чистая функция: тот же вход — тот же ответ.
// status == nullptr — от робота ещё ничего не было.
// low_battery_percent — ниже этого заряда незаряжающийся робот не готов (LOW_BATTERY).
// DELETING и потерю связи с dispatch учитывает AvailabilityTracker: в статусе робота этого нет.
fms::database::v1::RobotAvailability availability_of(const fms::v1::RobotStatus* status, double low_battery_percent, const MapStore& maps);

#endif //FLEETMANAGERSYSTEM_AVAILABILITY_HPP
