
#ifndef FLEETMANAGERSYSTEM_AVAILABILITY_HPP
#define FLEETMANAGERSYSTEM_AVAILABILITY_HPP
#pragma once

#include "fleet_database.pb.h"

// Может ли робот брать работу и почему нет — контракт UI, раздел 5.1. Чистая функция: тот же вход — тот же ответ.
// status == nullptr — от робота ещё ничего не было.
// low_battery_percent — ниже этого заряда незаряжающийся робот не готов (LOW_BATTERY).
// DELETING и потерю связи с dispatch учитывает AvailabilityTracker: в статусе робота этого нет.
fms::database::v1::RobotAvailability availability_of(const fms::v1::RobotStatus* status, double low_battery_percent);

#endif //FLEETMANAGERSYSTEM_AVAILABILITY_HPP
