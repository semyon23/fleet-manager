#ifndef FLEETMANAGERSYSTEM_ALARMRULES_HPP
#define FLEETMANAGERSYSTEM_ALARMRULES_HPP

#pragma once

#include <string>
#include <vector>

#include "fms_types.pb.h"

// Тревога, которую требует текущее состояние (M-5, контракт 8). Одна активная на key
struct AlarmCondition {
    std::string key;        // "ROBOT_ERROR/amr1/noRoute" — одна и та же причина
    std::string code;       // ROBOT_OFFLINE | EMERGENCY_STOP | ROBOT_ERROR | LOW_BATTERY
    std::string severity;   // WARNING | ERROR | CRITICAL
    std::string title;
    std::string detail;
    int delay_ms = 0;       // поднять, только если держится столько (обрыв связи на секунду — не тревога)
};

// Какие тревоги робота должны гореть сейчас. status == nullptr — от робота ещё ничего не было.
// Нет связи — только ROBOT_OFFLINE: остальное по устаревшему state не поднимаем (подавление по причине).
// Спит (HIBERNATING) — ничего: это штатно
std::vector<AlarmCondition> robot_alarm_conditions(const std::string& robot_id, const fms::v1::RobotStatus* status,
                                                   double low_battery_percent);

#endif //FLEETMANAGERSYSTEM_ALARMRULES_HPP