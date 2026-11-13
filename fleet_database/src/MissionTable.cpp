#include "../include/MissionTable.hpp"
#include <google/protobuf/util/time_util.h>

using google::protobuf::util::TimeUtil;

namespace
{
    constexpr const char* kColumns =
        "mission_id, map_id, robot_id, state, priority, body::text, "
        "(extract(epoch FROM created_at) * 1000)::bigint, "
        "(extract(epoch FROM updated_at) * 1000)::bigint, "
        "(extract(epoch FROM finished_at) * 1000)::bigint";

    dbv1::MissionRecord row_to_mission(pqxx::row_ref row)
    {
        dbv1::MissionRecord mission;
        mission.set_mission_id(row[0].as<std::string>());
        mission.set_map_id(row[1].as<std::string>());
        if (!row[2].is_null()) {
            mission.set_robot_id(row[2].as<std::string>());
        }
        mission.set_state(row[3].as<std::string>());
        mission.set_priority(row[4].as<uint32_t>());
        mission.set_body_json(row[5].as<std::string>());
        *mission.mutable_created_at() = TimeUtil::MillisecondsToTimestamp(row[6].as<long long>());
        *mission.mutable_updated_at() = TimeUtil::MillisecondsToTimestamp(row[7].as<long long>());
        if (!row[8].is_null()) {
            *mission.mutable_finished_at() = TimeUtil::MillisecondsToTimestamp(row[8].as<long long>());
        }
        return mission;
    }
} // namespace

void save_mission(pqxx::work& tx, const dbv1::MissionRecord& mission) {
    // Пустой robot_id и отсутствующий finished_at — NULL
    std::optional<std::string> robot_id;
    if (!mission.robot_id().empty()) {
        robot_id = mission.robot_id();
    }
    std::optional<long long> finished_at;
    if (mission.has_finished_at()) {
        finished_at = TimeUtil::TimestampToMilliseconds(mission.finished_at());
    }

    tx.exec("INSERT INTO missions (mission_id, map_id, robot_id, state, priority, body, "
            "created_at, updated_at, finished_at) "
            "VALUES ($1, $2, $3, $4, $5, $6::jsonb, "
            "to_timestamp($7::bigint / 1000.0), to_timestamp($8::bigint / 1000.0), "
            "to_timestamp($9::bigint / 1000.0)) "
            "ON CONFLICT (mission_id) DO UPDATE SET "
            "map_id = EXCLUDED.map_id, robot_id = EXCLUDED.robot_id, state = EXCLUDED.state, "
            "priority = EXCLUDED.priority, body = EXCLUDED.body, created_at = EXCLUDED.created_at, "
            "updated_at = EXCLUDED.updated_at, finished_at = EXCLUDED.finished_at",
            pqxx::params{mission.mission_id(), mission.map_id(), robot_id, mission.state(),
                         static_cast<int>(mission.priority()), mission.body_json(),
                         TimeUtil::TimestampToMilliseconds(mission.created_at()),
                         TimeUtil::TimestampToMilliseconds(mission.updated_at()), finished_at});
}

void insert_mission_events(pqxx::work& tx, const std::string& mission_id,
                           const google::protobuf::RepeatedPtrField<dbv1::MissionEvent>& events) {
    // Событий за одну запись — единицы, вставляем по одному
    for (const auto& event : events) {
        std::optional<std::string> robot_id;
        if (!event.robot_id().empty()) {
            robot_id = event.robot_id();
        }
        tx.exec("INSERT INTO mission_events (mission_id, at, robot_id, type, details) "
                "VALUES ($1, to_timestamp($2::bigint / 1000.0), $3, $4, $5::jsonb)",
                pqxx::params{mission_id, TimeUtil::TimestampToMilliseconds(event.at()), robot_id, event.type(),
                             event.details_json().empty() ? std::string("{}") : event.details_json()});
    }
}

std::optional<dbv1::MissionRecord> select_mission(pqxx::work& tx, const std::string& mission_id) {
    auto rows = tx.exec(std::string("SELECT ") + kColumns + " FROM missions WHERE mission_id = $1",
                        pqxx::params{mission_id});
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_mission(rows[0]);
}

std::vector<dbv1::MissionRecord> select_active_missions(pqxx::work& tx) {
    // То же условие, что у индекса missions_active, — запрос идёт по нему
    std::vector<dbv1::MissionRecord> missions;
    for (const auto& row : tx.exec(std::string("SELECT ") + kColumns + " FROM missions "
                                   "WHERE state IN ('QUEUED', 'ASSIGNED', 'RUNNING', 'PAUSED', 'CANCELING') "
                                   "ORDER BY created_at")) {
        missions.push_back(row_to_mission(row));
    }
    return missions;
}

std::vector<dbv1::MissionRecord> select_missions(pqxx::work& tx, const MissionFilter& filter) {
    // Один запрос на все сочетания фильтров: пустой фильтр отключает своё условие.
    // Курсор — сравнение пары, как в индексе missions_by_created (created_at DESC, mission_id DESC).
    // from/to — полуинтервал [from, to): соседние периоды не пересекаются
    std::vector<dbv1::MissionRecord> missions;
    for (const auto& row : tx.exec(
             std::string("SELECT ") + kColumns + " FROM missions "
             "WHERE ($1 = '' OR state = ANY(string_to_array($1, ','))) "
             "AND ($2 = '' OR robot_id = $2) "
             "AND ($3 = '' OR map_id = $3) "
             "AND ($4::bigint IS NULL OR (created_at, mission_id) < (to_timestamp($4::bigint / 1000.0), $5)) "
             "AND ($7::bigint IS NULL OR created_at >= to_timestamp($7::bigint / 1000.0)) "
             "AND ($8::bigint IS NULL OR created_at < to_timestamp($8::bigint / 1000.0)) "
             "ORDER BY created_at DESC, mission_id DESC LIMIT $6",
             pqxx::params{filter.states, filter.robot_id, filter.map_id, filter.before_created_ms,
                          filter.before_mission_id, filter.limit, filter.from_created_ms, filter.to_created_ms})) {
        missions.push_back(row_to_mission(row));
                          }
    return missions;
}
std::vector<std::string> select_mission_events(pqxx::work& tx, const std::string& mission_id, int limit) {
    // details || поля строки: при совпадении ключей побеждают поля строки — id, время и тип не подменить.
    // Время — ISO UTC с миллисекундами, как все времена контракта
    std::vector<std::string> events;
    for (const auto& row : tx.exec(
             R"(SELECT (details || jsonb_build_object(
                    'id', id,
                    'missionId', mission_id,
                    'at', to_char(at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
                    'robotId', robot_id,
                    'type', type))::text
                FROM mission_events WHERE mission_id = $1 ORDER BY id LIMIT $2)",
             pqxx::params{mission_id, limit})) {
        events.push_back(row[0].as<std::string>());
             }
    return events;
}