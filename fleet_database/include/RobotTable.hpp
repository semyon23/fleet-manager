
#ifndef FLEETMANAGERSYSTEM_ROBOTTABLE_HPP
#define FLEETMANAGERSYSTEM_ROBOTTABLE_HPP

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "fleet_database.pb.h"

namespace dbv1 = fms::database::v1;

std::optional<dbv1::RobotSpec> select_robot(pqxx::work& tx, const std::string& robot_id);
std::vector<dbv1::RobotSpec> select_robots(pqxx::work& tx, bool include_deleting);

// Бросает pqxx::unique_violation, если robot_id или пара manufacturer + serial_number заняты
dbv1::RobotSpec insert_robot(pqxx::work& tx, const dbv1::RobotSpec& spec);

// Меняет только name и labels. nullopt — такого робота нет
std::optional<dbv1::RobotSpec> update_robot(pqxx::work& tx, const dbv1::RobotSpec& spec);

// false — такого робота нет. Повторный вызов для PENDING_DELETE — не ошибка
bool set_robot_deleting(pqxx::work& tx, const std::string& robot_id);

enum class DeleteResult { Deleted, NotFound, NotDeleting };
DeleteResult delete_robot(pqxx::work& tx, const std::string& robot_id);   // только из PENDING_DELETE

#endif //FLEETMANAGERSYSTEM_ROBOTTABLE_HPP
