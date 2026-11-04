#include "MissionTable.hpp"
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