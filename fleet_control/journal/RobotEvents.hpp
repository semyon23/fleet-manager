#ifndef FLEETMANAGERSYSTEM_ROBOTEVENTS_HPP
#define FLEETMANAGERSYSTEM_ROBOTEVENTS_HPP

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "fleet_database.pb.h"
#include "fms_types.pb.h"

// Событие журнала (M-5): type и подробности. Источник, время и робота добавляет Journal
struct JournalEvent {
    std::string type;
    nlohmann::json details = nlohmann::json::object();
};

// Что поменялось у робота между двумя статусами: связь (CONNECTION) и ошибки VDA (ERROR — появилась / ушла).
// before == nullptr — первый статус робота: связь — если уже известна
std::vector<JournalEvent> robot_status_events(const fms::v1::RobotStatus* before, const fms::v1::RobotStatus& after);

// Сменилась готовность — AVAILABILITY {state, reasons}. before == nullptr (первый расчёт после старта) — нет события
std::optional<JournalEvent> availability_event(const fms::database::v1::RobotAvailability* before,
                                               const fms::database::v1::RobotAvailability& after);

#endif //FLEETMANAGERSYSTEM_ROBOTEVENTS_HPP