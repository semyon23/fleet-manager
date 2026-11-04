#ifndef FLEETMANAGERSYSTEM_APIJSON_HPP
#define FLEETMANAGERSYSTEM_APIJSON_HPP

#pragma once

#include <nlohmann/json.hpp>

#include "fleet_database.pb.h"

nlohmann::json robot_json(const fms::database::v1::RobotRecord& record);
nlohmann::json map_json(const fms::database::v1::MapRecord& map, bool full);
#endif //FLEETMANAGERSYSTEM_APIJSON_HPP
