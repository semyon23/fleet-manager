#ifndef FLEETMANAGERSYSTEM_EVENTTABLE_HPP
#define FLEETMANAGERSYSTEM_EVENTTABLE_HPP

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "fleet_database.pb.h"

namespace dbv1 = fms::database::v1;

// Журнал (M-5). Пустая строка или nullopt — условие отключено
struct EventFilter {
    std::string sources;                   // "ROBOT,ALARM"
    std::string types;                     // "CONNECTION,ERROR"
    std::string robot_id;
    std::string mission_id;
    std::string alarm_id;
    std::optional<long long> from_ms;      // at >= from
    std::optional<long long> to_ms;        // at <  to
    std::optional<long long> before_id;    // курсор: id последнего события прошлой страницы
    int limit = 50;
};

struct EventRow {
    long long id;
    std::string json;   // объект события для UI — собрала база
};

// События роботов, тревог, системы — по порядку. Пустые robot_id / mission_id / alarm_id — NULL
void insert_events(pqxx::work& tx, const google::protobuf::RepeatedPtrField<dbv1::Event>& events);

// Новые сверху (id ↓)
std::vector<EventRow> select_events(pqxx::work& tx, const EventFilter& filter);

#endif //FLEETMANAGERSYSTEM_EVENTTABLE_HPP