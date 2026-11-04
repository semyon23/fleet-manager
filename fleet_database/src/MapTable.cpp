#include "../include/MapTable.hpp"
#include <google/protobuf/util/time_util.h>

using google::protobuf::util::TimeUtil;

namespace
{
    constexpr const char* kColumns =
        "map_id, name, version, body::text, "
        "(extract(epoch FROM created_at) * 1000)::bigint, "
        "(extract(epoch FROM updated_at) * 1000)::bigint";

    dbv1::MapRecord row_to_map(pqxx::row_ref row)
    {
        dbv1::MapRecord map;
        map.set_map_id(row[0].as<std::string>());
        map.set_name(row[1].as<std::string>());
        map.set_version(row[2].as<uint32_t>());
        map.set_body_json(row[3].as<std::string>());
        *map.mutable_created_at() = TimeUtil::MillisecondsToTimestamp(row[4].as<long long>());
        *map.mutable_updated_at() = TimeUtil::MillisecondsToTimestamp(row[5].as<long long>());
        return map;
    }
} // namespace

std::vector<dbv1::MapRecord> select_maps(pqxx::work& tx) {
    std::vector<dbv1::MapRecord> maps;
    for (const auto& row : tx.exec(std::string("SELECT ") + kColumns + " FROM maps ORDER BY map_id")) {
        maps.push_back(row_to_map(row));
    }
    return maps;
}

std::optional<dbv1::MapRecord> select_map(pqxx::work& tx, const std::string& map_id) {
    auto rows = tx.exec(std::string("SELECT ") + kColumns + ", image FROM maps WHERE map_id = $1",
                        pqxx::params{map_id});
    if (rows.empty()) {
        return std::nullopt;
    }
    dbv1::MapRecord map = row_to_map(rows[0]);
    map.set_image(rows[0][6].as<std::string>());
    return map;
}

dbv1::MapRecord insert_map(pqxx::work& tx, const dbv1::MapRecord& map) {
    auto rows = tx.exec(std::string("INSERT INTO maps (map_id, name, body, image) "
                                    "VALUES ($1, $2, $3::jsonb, $4) RETURNING ") + kColumns,
                        pqxx::params{map.map_id(), map.name(), map.body_json(), map.image()});
    return row_to_map(rows[0]);
}

MapUpdateResult update_map(pqxx::work& tx, const dbv1::MapRecord& map, uint32_t expected_version,
                           const std::optional<std::string>& new_image) {
    // Проверка версии и запись — одним запросом: между ними никто не вклинится
    auto rows = tx.exec(std::string("UPDATE maps SET name = $2, body = $3::jsonb, image = COALESCE($4, image), "
                                    "version = version + 1, updated_at = now() "
                                    "WHERE map_id = $1 AND version = $5 RETURNING ") + kColumns,
                        pqxx::params{map.map_id(), map.name(), map.body_json(), new_image, expected_version});
    if (!rows.empty()) {
        return {MapUpdateResult::Updated, row_to_map(rows[0])};
    }
    // Не обновили: карты нет или её уже сохранили с другой версией
    auto exists = tx.exec("SELECT 1 FROM maps WHERE map_id = $1", pqxx::params{map.map_id()});
    return {exists.empty() ? MapUpdateResult::NotFound : MapUpdateResult::VersionConflict, {}};
}

bool delete_map(pqxx::work& tx, const std::string& map_id) {
    auto result = tx.exec("DELETE FROM maps WHERE map_id = $1", pqxx::params{map_id});
    return result.affected_rows() > 0;
}