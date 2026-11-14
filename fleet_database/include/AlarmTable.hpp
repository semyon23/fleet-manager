#ifndef FLEETMANAGERSYSTEM_ALARMTABLE_HPP
#define FLEETMANAGERSYSTEM_ALARMTABLE_HPP

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "fleet_database.pb.h"

namespace dbv1 = fms::database::v1;

// Список тревог для UI (контракт 8.2). Пустая строка или nullopt — условие отключено
struct AlarmFilter {
    std::optional<bool> active;
    std::optional<bool> acknowledged;
    std::string robot_id;
    std::string severities;                    // "ERROR,CRITICAL"
    std::optional<long long> before_created_ms;  // курсор, как у миссий: (createdAt, id)
    std::string before_alarm_id;
    int limit = 50;
};

struct AlarmRow {
    long long created_ms;
    std::string alarm_id;
    std::string json;   // объект Alarm контракта 8.1 — собрала база
};

// Создать или заменить целиком. Вторая активная с тем же key — unique_violation
void save_alarm(pqxx::work& tx, const dbv1::AlarmRecord& alarm);

// Активные или не подтверждённые — по created_at. Control читает при старте
std::vector<dbv1::AlarmRecord> select_open_alarms(pqxx::work& tx);

// Новые сверху (createdAt ↓, id ↓)
std::vector<AlarmRow> select_alarms(pqxx::work& tx, const AlarmFilter& filter);

std::optional<std::string> select_alarm_json(pqxx::work& tx, const std::string& alarm_id);

#endif //FLEETMANAGERSYSTEM_ALARMTABLE_HPP