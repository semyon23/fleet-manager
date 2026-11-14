#include "../include/EventTable.hpp"
#include <google/protobuf/util/time_util.h>

using google::protobuf::util::TimeUtil;

namespace {

std::optional<std::string> or_null(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

void insert_events(pqxx::work& tx, const google::protobuf::RepeatedPtrField<dbv1::Event>& events) {
    for (const auto& event : events) {
        tx.exec("INSERT INTO events (at, source, type, mission_id, robot_id, alarm_id, details) "
                "VALUES (to_timestamp($1::bigint / 1000.0), $2, $3, $4, $5, $6, $7::jsonb)",
                pqxx::params{TimeUtil::TimestampToMilliseconds(event.at()), event.source(), event.type(),
                             or_null(event.mission_id()), or_null(event.robot_id()), or_null(event.alarm_id()),
                             event.details_json().empty() ? std::string("{}") : event.details_json()});
    }
}

std::vector<EventRow> select_events(pqxx::work& tx, const EventFilter& filter) {
    // Один запрос на все сочетания фильтров, как у миссий. details || поля строки: поля строки побеждают.
    // Фильтр по роботу или миссии идёт по индексу (robot_id, id) / (mission_id, id) сразу в порядке id ↓
    std::vector<EventRow> events;
    for (const auto& row : tx.exec(
             R"(SELECT id, (details || jsonb_build_object(
                    'id', id,
                    'at', to_char(at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
                    'source', source,
                    'type', type,
                    'robotId', robot_id,
                    'missionId', mission_id,
                    'alarmId', alarm_id))::text
                FROM events
                WHERE ($1 = '' OR source = ANY(string_to_array($1, ',')))
                  AND ($2 = '' OR type = ANY(string_to_array($2, ',')))
                  AND ($3 = '' OR robot_id = $3)
                  AND ($4 = '' OR mission_id = $4)
                  AND ($5 = '' OR alarm_id = $5)
                  AND ($6::bigint IS NULL OR at >= to_timestamp($6::bigint / 1000.0))
                  AND ($7::bigint IS NULL OR at < to_timestamp($7::bigint / 1000.0))
                  AND ($8::bigint IS NULL OR id < $8)
                ORDER BY id DESC LIMIT $9)",
             pqxx::params{filter.sources, filter.types, filter.robot_id, filter.mission_id, filter.alarm_id,
                          filter.from_ms, filter.to_ms, filter.before_id, filter.limit})) {
        events.push_back({row[0].as<long long>(), row[1].as<std::string>()});
    }
    return events;
}