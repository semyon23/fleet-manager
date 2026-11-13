#ifndef FLEETMANAGERSYSTEM_MISSIONTABLE_HPP
#define FLEETMANAGERSYSTEM_MISSIONTABLE_HPP

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "fleet_database.pb.h"

namespace dbv1 = fms::database::v1;

struct MissionFilter {
    std::string states;
    std::string robot_id;
    std::string map_id;
    std::optional<long long> from_created_ms;   // createdAt >= from (c3, History)
    std::optional<long long> to_created_ms;     // createdAt <  to
    std::optional<long long> before_created_ms;
    std::string before_mission_id;
    int limit = 50;
};


void save_mission(pqxx::work& tx, const dbv1::MissionRecord& mission);

// События миссии (c3) — по порядку. В одной транзакции с save_mission и после неё: внешний ключ на missions.
// Пустой details_json — {}
void insert_mission_events(pqxx::work& tx, const std::string& mission_id,
                           const google::protobuf::RepeatedPtrField<dbv1::MissionEvent>& events);

std::optional<dbv1::MissionRecord> select_mission(pqxx::work& tx, const std::string& mission_id);

std::vector<dbv1::MissionRecord> select_active_missions(pqxx::work& tx);

std::vector<dbv1::MissionRecord> select_missions(pqxx::work& tx, const MissionFilter& filter);

// События миссии для UI: каждое — готовый объект JSON контракта (id, missionId, at, robotId, type + details),
// по порядку, не больше limit
std::vector<std::string> select_mission_events(pqxx::work& tx, const std::string& mission_id, int limit);

#endif //FLEETMANAGERSYSTEM_MISSIONTABLE_HPP