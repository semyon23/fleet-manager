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
    std::optional<long long> before_created_ms;
    std::string before_mission_id;
    int limit = 50;
};


void save_mission(pqxx::work& tx, const dbv1::MissionRecord& mission);

std::optional<dbv1::MissionRecord> select_mission(pqxx::work& tx, const std::string& mission_id);

std::vector<dbv1::MissionRecord> select_active_missions(pqxx::work& tx);

std::vector<dbv1::MissionRecord> select_missions(pqxx::work& tx, const MissionFilter& filter);

#endif //FLEETMANAGERSYSTEM_MISSIONTABLE_HPP
