
#include "../include/RobotTable.hpp"
#include <google/protobuf/util/time_util.h>
#include <map>
#include <nlohmann/json.hpp>
#include <google/protobuf/util/json_util.h>
#include <stdexcept>

using google::protobuf::util::TimeUtil;

namespace
{
    // Столбцы в том порядке, в каком их читает row_to_spec.
    // labels — как текст JSON, время — миллисекунды unix.


    constexpr const char* kColumns =
        "robot_id, manufacturer, serial_number, name, labels::text, lifecycle, "
        "(extract(epoch FROM created_at) * 1000)::bigint, "
        "(extract(epoch FROM updated_at) * 1000)::bigint";

    dbv1::RobotLifecycle lifecycle_from_text(const std::string& text)
    {
        if (text == "ACTIVE") return dbv1::ROBOT_LIFECYCLE_ACTIVE;
        if (text == "PENDING_DELETE") return dbv1::ROBOT_LIFECYCLE_PENDING_DELETE;
        return dbv1::ROBOT_LIFECYCLE_UNSPECIFIED;
    }

    dbv1::RobotSpec row_to_spec(pqxx::row_ref row)
    {
        dbv1::RobotSpec spec;
        spec.set_robot_id(row[0].as<std::string>());
        spec.set_manufacturer(row[1].as<std::string>());
        spec.set_serial_number(row[2].as<std::string>());
        spec.set_name(row[3].as<std::string>());
        for (const auto& [key, value] : nlohmann::json::parse(row[4].as<std::string>()).items())
        {
            (*spec.mutable_labels())[key] = value.get<std::string>();
        }
        spec.set_lifecycle(lifecycle_from_text(row[5].as<std::string>()));
        *spec.mutable_created_at() = TimeUtil::MillisecondsToTimestamp(row[6].as<long long>());
        *spec.mutable_updated_at() = TimeUtil::MillisecondsToTimestamp(row[7].as<long long>());
        return spec;
    }


    std::string labels_json(const dbv1::RobotSpec& spec)
    {
        std::map<std::string, std::string> labels(spec.labels().begin(), spec.labels().end());
        return nlohmann::json(labels).dump();
    }

    const std::string kSelectRecord = std::string("SELECT ") + kColumns +
        ", robot_status.status::text FROM robots LEFT JOIN robot_status USING (robot_id)";

    std::string status_to_json(const fms::v1::RobotStatus& status)
    {
        std::string json;
        if (!google::protobuf::util::MessageToJsonString(status, &json).ok())
        {
            throw std::runtime_error("RobotStatus -> JSON failed");
        }
        return json;
    }

    fms::v1::RobotStatus status_from_json(const std::string& json)
    {
        fms::v1::RobotStatus status;
        google::protobuf::util::JsonParseOptions options;
        options.ignore_unknown_fields = true; // поле удалили из proto — старые записи всё равно читаются
        if (!google::protobuf::util::JsonStringToMessage(json, &status, options).ok())
        {
            throw std::runtime_error("robot_status: stored JSON does not match RobotStatus");
        }
        return status;
    }

    dbv1::RobotRecord row_to_record(pqxx::row_ref row)
    {
        dbv1::RobotRecord record;
        *record.mutable_spec() = row_to_spec(row);
        if (!row[8].is_null())
        {
            *record.mutable_status() = status_from_json(row[8].as<std::string>());
        }
        return record;
    }
} // namespace

std::optional<dbv1::RobotRecord> select_robot(pqxx::work& tx, const std::string& robot_id) {
    auto rows = tx.exec(kSelectRecord + " WHERE robot_id = $1", pqxx::params{robot_id});
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_record(rows[0]);
}

std::vector<dbv1::RobotRecord> select_robots(pqxx::work& tx, bool include_deleting) {
    std::string sql = kSelectRecord;
    if (!include_deleting) {
        sql += " WHERE lifecycle = 'ACTIVE'";
    }
    sql += " ORDER BY robot_id";

    std::vector<dbv1::RobotRecord> robots;
    for (const auto& row : tx.exec(sql)) {
        robots.push_back(row_to_record(row));
    }
    return robots;
}

dbv1::RobotSpec insert_robot(pqxx::work& tx, const dbv1::RobotSpec& spec) {
    auto rows = tx.exec(std::string("INSERT INTO robots (robot_id, manufacturer, serial_number, name, labels) "
                                    "VALUES ($1, $2, $3, $4, $5::jsonb) RETURNING ") + kColumns,
                        pqxx::params{spec.robot_id(), spec.manufacturer(), spec.serial_number(), spec.name(),
                                     labels_json(spec)});
    return row_to_spec(rows[0]);
}

std::optional<dbv1::RobotSpec> update_robot(pqxx::work& tx, const dbv1::RobotSpec& spec) {
    auto rows = tx.exec(std::string("UPDATE robots SET name = $2, labels = $3::jsonb, updated_at = now() "
                                    "WHERE robot_id = $1 RETURNING ") + kColumns,
                        pqxx::params{spec.robot_id(), spec.name(), labels_json(spec)});
    if (rows.empty()) {
        return std::nullopt;
    }
    return row_to_spec(rows[0]);
}

bool set_robot_deleting(pqxx::work& tx, const std::string& robot_id) {
    auto result = tx.exec("UPDATE robots SET lifecycle = 'PENDING_DELETE', updated_at = now() WHERE robot_id = $1",
                          pqxx::params{robot_id});
    return result.affected_rows() > 0;
}

DeleteResult delete_robot(pqxx::work& tx, const std::string& robot_id) {
    auto result = tx.exec("DELETE FROM robots WHERE robot_id = $1 AND lifecycle = 'PENDING_DELETE'",
                          pqxx::params{robot_id});
    if (result.affected_rows() > 0) {
        return DeleteResult::Deleted;
    }
    // Не удалили: робота нет или он не помечен на удаление
    auto rows = tx.exec("SELECT 1 FROM robots WHERE robot_id = $1", pqxx::params{robot_id});
    return rows.empty() ? DeleteResult::NotFound : DeleteResult::NotDeleting;
}

void save_robot_status(pqxx::work& tx, const fms::v1::RobotStatus& status) {
    tx.exec("INSERT INTO robot_status (robot_id, status) VALUES ($1, $2::jsonb) "
            "ON CONFLICT (robot_id) DO UPDATE SET status = excluded.status, saved_at = now()",
            pqxx::params{status.robot_id(), status_to_json(status)});
}

void save_factsheet(pqxx::work& tx, const std::string& robot_id, const std::string& factsheet_json,
                    long long received_at_ms) {
    tx.exec("INSERT INTO robot_factsheets (robot_id, factsheet, received_at) "
            "VALUES ($1, $2::jsonb, to_timestamp($3::bigint / 1000.0)) "
            "ON CONFLICT (robot_id) DO UPDATE SET factsheet = excluded.factsheet, received_at = excluded.received_at",
            pqxx::params{robot_id, factsheet_json, received_at_ms});
}

std::optional<dbv1::GetFactsheetResponse> select_factsheet(pqxx::work& tx, const std::string& robot_id) {
    auto rows = tx.exec("SELECT factsheet::text, (extract(epoch FROM received_at) * 1000)::bigint "
                        "FROM robot_factsheets WHERE robot_id = $1",
                        pqxx::params{robot_id});
    if (rows.empty()) {
        return std::nullopt;
    }
    dbv1::GetFactsheetResponse factsheet;
    factsheet.set_factsheet_json(rows[0][0].as<std::string>());
    *factsheet.mutable_received_at() = TimeUtil::MillisecondsToTimestamp(rows[0][1].as<long long>());
    return factsheet;
}