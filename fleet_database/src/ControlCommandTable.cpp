#include "../include/ControlCommandTable.hpp"

#include <nlohmann/json.hpp>

#include "fleet_dispatch.pb.h"

namespace db = fms::database::v1;
namespace dp = fms::dispatch::v1;

namespace {

std::string hex(const std::string& bytes) {
    constexpr char digits[] = "0123456789abcdef";

    std::string result;
    result.reserve(bytes.size() * 2);

    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }

    return result;
}

std::string unhex(const std::string& encoded) {
    if (encoded.size() % 2) {
        throw std::runtime_error(
            "invalid stored command hex");
    }

    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;

        throw std::runtime_error(
            "invalid stored command hex digit");
    };

    std::string result;
    result.reserve(encoded.size() / 2);

    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        result.push_back(static_cast<char>(
            (digit(encoded[i]) << 4) |
            digit(encoded[i + 1])));
    }

    return result;
}

} // namespace

std::string control_command_error(
    const db::ControlCommandRecord& c) {

    if (c.command_id().empty() ||
        c.robot_id().empty() ||
        c.mission_id().empty() ||
        c.map_id().empty() ||
        c.map_version() == 0 ||
        c.recovery_version() != 1 ||
        c.request_proto().empty()) {
        return "missing command identity, version or payload";
    }

    const auto recovery = nlohmann::json::parse(
        c.recovery_json(), nullptr, false);

    if (!recovery.is_object()) {
        return "recovery_json must be an object";
    }

    if (c.kind() != db::CONTROL_COMMAND_KIND_EXTEND_MISSION &&
        (!c.move_node_id().empty() || c.revision() != 0)) {
        return "only EXTEND has a move_node_id and revision";
    }

    switch (c.kind()) {
        case db::CONTROL_COMMAND_KIND_ADD_MISSION: {
            dp::AddMissionRequest request;

            if (!request.ParseFromString(c.request_proto()) ||
                request.mission().mission_id() != c.mission_id() ||
                request.mission().robot_id() != c.robot_id() ||
                request.mission().nodes_size() == 0) {
                return "ADD payload identity or steps are invalid";
            }

            break;
        }

        case db::CONTROL_COMMAND_KIND_EXTEND_MISSION: {
            dp::ExtendMissionRequest request;

            if (c.move_node_id().empty() ||
                c.revision() == 0 ||
                !request.ParseFromString(c.request_proto()) ||
                request.mission_id() != c.mission_id() ||
                request.move_node_id() != c.move_node_id() ||
                request.revision() != c.revision() ||
                !request.has_released_nodes() ||
                request.released_nodes() == 0) {
                return
                    "EXTEND payload identity, revision or release are invalid";
            }

            break;
        }

        case db::CONTROL_COMMAND_KIND_CANCEL_MISSION: {
            dp::CancelMissionRequest request;

            if (!request.ParseFromString(c.request_proto()) ||
                request.mission_id() != c.mission_id()) {
                return "CANCEL payload identity is invalid";
            }

            break;
        }

        default:
            return "unsupported command kind";
    }

    return {};
}

bool put_control_command(
    pqxx::work& tx,
    const db::ControlCommandRecord& c) {

    const auto bytes = hex(c.SerializeAsString());

    tx.exec(
        "INSERT INTO control_commands "
        "(command_id, robot_id, mission_id, map_id, map_version, "
        "kind, move_node_id, revision, payload) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,decode($9,'hex')) "
        "ON CONFLICT DO NOTHING",
        pqxx::params{
            c.command_id(),
            c.robot_id(),
            c.mission_id(),
            c.map_id(),
            static_cast<long long>(c.map_version()),
            static_cast<int>(c.kind()),
            c.move_node_id(),
            static_cast<long long>(c.revision()),
            bytes
        });

    const auto rows = tx.exec(
        "SELECT encode(payload,'hex'), resolved "
        "FROM control_commands WHERE command_id=$1",
        pqxx::params{c.command_id()});

    if (rows.empty() ||
        rows[0][0].as<std::string>() != bytes) {
        throw ControlCommandConflict(
            "command id or logical revision already has other data");
    }

    return rows[0][1].as<bool>();
}

std::vector<db::StoredControlCommand>
list_control_commands(
    pqxx::work& tx,
    const std::string& robot_id,
    bool include_resolved) {

    const auto rows = tx.exec(
        "SELECT encode(payload,'hex'), resolved "
        "FROM control_commands "
        "WHERE ($1='' OR robot_id=$1) "
        "AND ($2 OR NOT resolved) "
        "ORDER BY sequence_id",
        pqxx::params{robot_id, include_resolved});

    std::vector<db::StoredControlCommand> result;
    result.reserve(rows.size());

    for (const auto& row : rows) {
        db::StoredControlCommand item;

        if (!item.mutable_command()->ParseFromString(
                unhex(row[0].as<std::string>()))) {
            throw std::runtime_error(
                "stored control command is corrupt");
        }

        item.set_resolved(row[1].as<bool>());
        result.push_back(std::move(item));
    }

    return result;
}

bool resolve_control_command(
    pqxx::work& tx,
    const std::string& id) {

    return !tx.exec(
        "UPDATE control_commands "
        "SET resolved=true, "
        "resolved_at=COALESCE(resolved_at,now()) "
        "WHERE command_id=$1 "
        "RETURNING command_id",
        pqxx::params{id}).empty();
}