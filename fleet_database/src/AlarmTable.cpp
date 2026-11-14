#include "../include/AlarmTable.hpp"
#include <google/protobuf/util/time_util.h>

using google::protobuf::util::TimeUtil;

namespace {

constexpr const char* kColumns =
    "alarm_id, code, severity, source, robot_id, mission_id, title, detail, key, active, "
    "(extract(epoch FROM created_at) * 1000)::bigint, "
    "(extract(epoch FROM cleared_at) * 1000)::bigint, "
    "(extract(epoch FROM acknowledged_at) * 1000)::bigint, "
    "acknowledged_by";

// Alarm контракта 8.1. Времена — ISO UTC с миллисекундами; NULL → null
constexpr const char* kJson = R"(jsonb_build_object(
    'id', alarm_id,
    'severity', severity,
    'source', source,
    'code', code,
    'title', title,
    'detail', detail,
    'robotId', robot_id,
    'missionId', mission_id,
    'active', active,
    'createdAt', to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
    'clearedAt', to_char(cleared_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
    'acknowledgedAt', to_char(acknowledged_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
    'acknowledgedBy', acknowledged_by)::text)";

std::optional<std::string> or_null(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<long long> ms_or_null(bool present, const google::protobuf::Timestamp& time) {
    if (!present) {
        return std::nullopt;
    }
    return TimeUtil::TimestampToMilliseconds(time);
}

dbv1::AlarmRecord row_to_alarm(pqxx::row_ref row) {
    dbv1::AlarmRecord alarm;
    alarm.set_alarm_id(row[0].as<std::string>());
    alarm.set_code(row[1].as<std::string>());
    alarm.set_severity(row[2].as<std::string>());
    alarm.set_source(row[3].as<std::string>());
    if (!row[4].is_null()) alarm.set_robot_id(row[4].as<std::string>());
    if (!row[5].is_null()) alarm.set_mission_id(row[5].as<std::string>());
    alarm.set_title(row[6].as<std::string>());
    alarm.set_detail(row[7].as<std::string>());
    alarm.set_key(row[8].as<std::string>());
    alarm.set_active(row[9].as<bool>());
    *alarm.mutable_created_at() = TimeUtil::MillisecondsToTimestamp(row[10].as<long long>());
    if (!row[11].is_null()) {
        *alarm.mutable_cleared_at() = TimeUtil::MillisecondsToTimestamp(row[11].as<long long>());
    }
    if (!row[12].is_null()) {
        *alarm.mutable_acknowledged_at() = TimeUtil::MillisecondsToTimestamp(row[12].as<long long>());
    }
    if (!row[13].is_null()) alarm.set_acknowledged_by(row[13].as<std::string>());
    return alarm;
}

}  // namespace

void save_alarm(pqxx::work& tx, const dbv1::AlarmRecord& alarm) {
    tx.exec("INSERT INTO alarms (alarm_id, code, severity, source, robot_id, mission_id, title, detail, key, "
            "active, created_at, cleared_at, acknowledged_at, acknowledged_by) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, to_timestamp($11::bigint / 1000.0), "
            "to_timestamp($12::bigint / 1000.0), to_timestamp($13::bigint / 1000.0), $14) "
            "ON CONFLICT (alarm_id) DO UPDATE SET "
            "code = EXCLUDED.code, severity = EXCLUDED.severity, source = EXCLUDED.source, "
            "robot_id = EXCLUDED.robot_id, mission_id = EXCLUDED.mission_id, title = EXCLUDED.title, "
            "detail = EXCLUDED.detail, key = EXCLUDED.key, active = EXCLUDED.active, "
            "created_at = EXCLUDED.created_at, cleared_at = EXCLUDED.cleared_at, "
            "acknowledged_at = EXCLUDED.acknowledged_at, acknowledged_by = EXCLUDED.acknowledged_by",
            pqxx::params{alarm.alarm_id(), alarm.code(), alarm.severity(), alarm.source(),
                         or_null(alarm.robot_id()), or_null(alarm.mission_id()), alarm.title(), alarm.detail(),
                         alarm.key(), alarm.active(), TimeUtil::TimestampToMilliseconds(alarm.created_at()),
                         ms_or_null(alarm.has_cleared_at(), alarm.cleared_at()),
                         ms_or_null(alarm.has_acknowledged_at(), alarm.acknowledged_at()),
                         or_null(alarm.acknowledged_by())});
}

std::vector<dbv1::AlarmRecord> select_open_alarms(pqxx::work& tx) {
    // То же условие, что у индекса alarms_open
    std::vector<dbv1::AlarmRecord> alarms;
    for (const auto& row : tx.exec(std::string("SELECT ") + kColumns + " FROM alarms "
                                   "WHERE active OR acknowledged_at IS NULL ORDER BY created_at")) {
        alarms.push_back(row_to_alarm(row));
    }
    return alarms;
}

std::vector<AlarmRow> select_alarms(pqxx::work& tx, const AlarmFilter& filter) {
    std::vector<AlarmRow> alarms;
    for (const auto& row : tx.exec(
             std::string("SELECT (extract(epoch FROM created_at) * 1000)::bigint, alarm_id, ") + kJson + " FROM alarms "
             "WHERE ($1::boolean IS NULL OR active = $1) "
             "AND ($2::boolean IS NULL OR (acknowledged_at IS NOT NULL) = $2) "
             "AND ($3 = '' OR robot_id = $3) "
             "AND ($4 = '' OR severity = ANY(string_to_array($4, ','))) "
             "AND ($5::bigint IS NULL OR (created_at, alarm_id) < (to_timestamp($5::bigint / 1000.0), $6)) "
             "ORDER BY created_at DESC, alarm_id DESC LIMIT $7",
             pqxx::params{filter.active, filter.acknowledged, filter.robot_id, filter.severities,
                          filter.before_created_ms, filter.before_alarm_id, filter.limit})) {
        alarms.push_back({row[0].as<long long>(), row[1].as<std::string>(), row[2].as<std::string>()});
    }
    return alarms;
}

std::optional<std::string> select_alarm_json(pqxx::work& tx, const std::string& alarm_id) {
    auto rows = tx.exec(std::string("SELECT ") + kJson + " FROM alarms WHERE alarm_id = $1", pqxx::params{alarm_id});
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows[0][0].as<std::string>();
}