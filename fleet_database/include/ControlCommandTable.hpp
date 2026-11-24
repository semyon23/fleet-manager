#ifndef FLEETMANAGERSYSTEM_CONTROLCOMMANDTABLE_HPP
#define FLEETMANAGERSYSTEM_CONTROLCOMMANDTABLE_HPP
#pragma once

#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <vector>

#include "fleet_database.pb.h"

struct ControlCommandConflict : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string control_command_error(
    const fms::database::v1::ControlCommandRecord& command);

// Возвращает resolved существующей/новой записи.
// Повтор должен быть идентичен побайтно.
//
// Конфликт сохраняет исходную запись.
// Транзакцией владеет Database::run.
bool put_control_command(
    pqxx::work& tx,
    const fms::database::v1::ControlCommandRecord& command);

std::vector<fms::database::v1::StoredControlCommand>
list_control_commands(
    pqxx::work& tx,
    const std::string& robot_id,
    bool include_resolved, const std::string& mission_id);

void list_open_control_missions(pqxx::work& tx, const std::string& after,
                                fms::database::v1::ListOpenControlMissionsResponse& response);

void close_mission_commands(pqxx::work& tx, const fms::database::v1::CloseMissionCommandsRequest& request);

// false — нет такого command_id.
// Повтор завершения записи идемпотентен.
//
// Это изменение журнала.
// Реальные hard-резервы функция не освобождает.
bool resolve_control_command(
    pqxx::work& tx,
    const std::string& command_id);


#endif //FLEETMANAGERSYSTEM_CONTROLCOMMANDTABLE_HPP
