#ifndef FLEETMANAGERSYSTEM_MISSIONTABLE_HPP
#define FLEETMANAGERSYSTEM_MISSIONTABLE_HPP

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "fleet_database.pb.h"

namespace dbv1 = fms::database::v1;

// M-3. Миссии пишет control (ControlStorage). body_json — объект Mission контракта UI,
// остальные поля — его копии для фильтров. Времена ставит control, база хранит как есть.

// Создать или заменить целиком (по mission_id).
// pqxx::data_exception — body_json не JSON; pqxx::check_violation — неверный state или priority
void save_mission(pqxx::work& tx, const dbv1::MissionRecord& mission);

std::optional<dbv1::MissionRecord> select_mission(pqxx::work& tx, const std::string& mission_id);

// QUEUED, ASSIGNED, RUNNING, PAUSED, CANCELING — по created_at
std::vector<dbv1::MissionRecord> select_active_missions(pqxx::work& tx);



#endif //FLEETMANAGERSYSTEM_MISSIONTABLE_HPP
