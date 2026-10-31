
#ifndef FLEETMANAGERSYSTEM_ROBOTTABLE_HPP
#define FLEETMANAGERSYSTEM_ROBOTTABLE_HPP

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "fleet_database.pb.h"

namespace dbv1 = fms::database::v1;

std::optional<dbv1::RobotRecord> select_robot(pqxx::work& tx, const std::string& robot_id);
std::vector<dbv1::RobotRecord> select_robots(pqxx::work& tx, bool include_deleting);


// Бросает pqxx::unique_violation, если robot_id или пара manufacturer + serial_number заняты
dbv1::RobotSpec insert_robot(pqxx::work& tx, const dbv1::RobotSpec& spec);

// Меняет только name и labels. nullopt — такого робота нет
std::optional<dbv1::RobotSpec> update_robot(pqxx::work& tx, const dbv1::RobotSpec& spec);

// false — такого робота нет. Повторный вызов для PENDING_DELETE — не ошибка
bool set_robot_deleting(pqxx::work& tx, const std::string& robot_id);

enum class DeleteResult { Deleted, NotFound, NotDeleting };
DeleteResult delete_robot(pqxx::work& tx, const std::string& robot_id);   // только из PENDING_DELETE


// DB-3. Обе записи бросают pqxx::foreign_key_violation, если такого робота нет
void save_robot_status(pqxx::work& tx, const fms::v1::RobotStatus& status);
void save_factsheet(pqxx::work& tx, const std::string& robot_id, const std::string& factsheet_json, long long received_at_ms);   // не JSON — бросает pqxx::data_exception
std::optional<dbv1::GetFactsheetResponse> select_factsheet(pqxx::work& tx, const std::string& robot_id);

#endif //FLEETMANAGERSYSTEM_ROBOTTABLE_HPP
