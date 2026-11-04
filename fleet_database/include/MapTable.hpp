#ifndef FLEETMANAGERSYSTEM_MAPTABLE_HPP
#define FLEETMANAGERSYSTEM_MAPTABLE_HPP

#pragma once

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "fleet_database.pb.h"

namespace dbv1 = fms::database::v1;

// M-1. Карты пишет control (ControlStorage). body_json — документ редактора UI,
// image — картинка карты (data URL), читается только с одной картой.

// Все карты с body_json, без image — по map_id
std::vector<dbv1::MapRecord> select_maps(pqxx::work& tx);

// Карта целиком, с image
std::optional<dbv1::MapRecord> select_map(pqxx::work& tx, const std::string& map_id);

// version = 1. Бросает pqxx::unique_violation, если map_id занят, pqxx::data_exception — body_json не JSON
dbv1::MapRecord insert_map(pqxx::work& tx, const dbv1::MapRecord& map);

struct MapUpdateResult
{
    enum Result { Updated, NotFound, VersionConflict } result;
    dbv1::MapRecord map;   // только для Updated, без image
};

// Меняет name, body_json и, если задан new_image, картинку; version + 1.
// Только если в базе всё ещё expected_version. pqxx::data_exception — body_json не JSON
MapUpdateResult update_map(pqxx::work& tx, const dbv1::MapRecord& map, uint32_t expected_version,
                           const std::optional<std::string>& new_image);


bool delete_map(pqxx::work& tx, const std::string& map_id);

#endif //FLEETMANAGERSYSTEM_MAPTABLE_HPP
