#ifndef FLEETMANAGERSYSTEM_EXECUTIONHEAD_HPP
#define FLEETMANAGERSYSTEM_EXECUTIONHEAD_HPP

#pragma once

#include "ExecutionCheckpoint.hpp"
#include "ExecutionVersion.hpp"
#include "RobotTable.hpp"
#include <boost/algorithm/hex.hpp>
#include <google/protobuf/util/message_differencer.h>
#include <grpcpp/support/status.h>
#include <iterator>
#include <stdexcept>

namespace execution_head {

    inline std::string decode(const pqxx::field_ref& field) {
        const auto hex = field.as<std::string>();
        std::string bytes;
        boost::algorithm::unhex(hex.begin(), hex.end(), std::back_inserter(bytes));
        return bytes;
    }

    inline dbv1::MissionExecution read(const pqxx::field_ref& field) {
        dbv1::MissionExecution execution;
        if (!execution.ParseFromString(decode(field))) {
            throw std::runtime_error("corrupt dispatch execution");
        }
        return execution;
    }

inline bool terminal(fms::v1::MissionState state) {
    using namespace fms::v1;
    return state == MISSION_STATE_COMPLETED ||
           state == MISSION_STATE_FAILED ||
           state == MISSION_STATE_CANCELED;
}

inline void save(pqxx::work& tx, const dbv1::MissionExecution& e,
                 const std::string& hex) {
    const auto& mid = e.mission().mission_id();
    const auto& robot = e.mission().robot_id();

    tx.exec("INSERT INTO dispatch_execution_heads(mission_id,robot_id) VALUES ($1,$2) "
            "ON CONFLICT DO NOTHING", pqxx::params{mid, robot});

    const auto rows = tx.exec(
        "SELECT version,terminal,robot_id,encode(execution,'hex') "
        "FROM dispatch_execution_heads WHERE mission_id=$1 FOR UPDATE",
        pqxx::params{mid});
    const auto row = rows.at(0);
    const auto stored = row[0].as<std::uint64_t>();

    if (e.checkpoint_version() == 0) {
        if (stored != 0) {
            throw grpc::Status{grpc::StatusCode::FAILED_PRECONDITION, "unversioned writer"};
        }
        return;
    }

    const auto stored_robot = row[2].as<std::string>();
    if (!stored_robot.empty() && stored_robot != robot) {
        throw grpc::Status{grpc::StatusCode::ALREADY_EXISTS,
                           "mission belongs to another robot"};
    }

    using namespace execution_version;
    switch (decide(stored, row[1].as<bool>(), e.checkpoint_version())) {
    case Decision::replay:
        if (google::protobuf::util::MessageDifferencer::Equals(read(row[3]), e)) return;
        throw grpc::Status{grpc::StatusCode::ALREADY_EXISTS, "conflicting checkpoint"};
    case Decision::closed:
    case Decision::stale:
        throw grpc::Status{grpc::StatusCode::FAILED_PRECONDITION, "checkpoint superseded"};
    case Decision::advance:
        break;
    }

    const auto state = e.status().state();
    const bool closed = terminal(state);
    const bool uncertain = closed && state != fms::v1::MISSION_STATE_COMPLETED &&
                           !execution_checkpoint::confirmed_cancel(e);

    tx.exec("UPDATE dispatch_execution_heads SET robot_id=$2,version=$3,terminal=$4,"
            "uncertain=$5,execution=decode($6,'hex') WHERE mission_id=$1",
            pqxx::params{mid, robot, e.checkpoint_version(), closed, uncertain, hex});

    if (e.has_order()) {
        tx.exec("UPDATE dispatch_execution_heads SET grant_step=$2,grant_order=$3,"
                "grant_update=$4 WHERE mission_id=$1",
                pqxx::params{mid, e.order().mission_node_id(), e.order().order_id(),
                             e.order().order_update_id()});
    }
}

inline bool lookup(pqxx::work& tx, const std::string& mission_id,
                   fms::v1::GetExecutionCheckpointResponse& result) {
    const auto rows = tx.exec(
        "SELECT h.robot_id,h.version,h.terminal,h.uncertain,"
        "encode(h.execution,'hex'),r.attempt "
        "FROM dispatch_execution_heads h "
        "LEFT JOIN dispatch_add_receipts r USING (mission_id) "
        "WHERE h.mission_id=$1", pqxx::params{mission_id});
    if (rows.empty()) return false;

    const auto row = rows.at(0);
    const auto robot = row[0].as<std::string>();
    const auto version = row[1].as<std::uint64_t>();
    const bool terminal = row[2].as<bool>();
    const bool uncertain = row[3].as<bool>();
    result.set_mission_id(mission_id);
    result.set_robot_id(robot);
    result.set_checkpoint_version(version);
    if (!row[5].is_null()) result.set_add_attempt(row[5].as<std::uint32_t>());

    if (row[4].is_null()) {
        if (version != 0 || terminal || !uncertain)
            throw std::runtime_error("missing dispatch checkpoint payload");
        result.set_kind(fms::v1::GetExecutionCheckpointResponse::UNCERTAIN);
        return true;
    }

    auto e = read(row[4]);
    if (version == 0 || version != e.checkpoint_version() || robot.empty() ||
        e.mission().mission_id() != mission_id || e.status().mission_id() != mission_id ||
        e.mission().robot_id() != robot || e.status().robot_id() != robot) {
        throw std::runtime_error("dispatch checkpoint identity mismatch");
    }
    result.set_kind(execution_checkpoint::classify(e, terminal, uncertain));
    result.mutable_status()->Swap(e.mutable_status());
    return true;
}

inline void load(pqxx::work& tx, dbv1::DispatchSnapshot& snapshot) {
    tx.exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY");

    for (auto& robot : select_robots(tx, true)) {
        snapshot.add_robots()->Swap(&robot);
    }

    const auto rows = tx.exec(
        "SELECT h.mission_id,h.robot_id,h.uncertain,encode(h.execution,'hex'),"
        "encode(o.execution,'hex'),CASE WHEN h.uncertain THEN encode(r.request_bytes,'hex') END "
        "FROM dispatch_execution_heads h LEFT JOIN dispatch_execution_orders o "
        "ON (h.mission_id,h.grant_step,h.grant_order,h.grant_update)="
        "(o.mission_id,o.step_id,o.order_id,o.update_id) "
        "LEFT JOIN dispatch_add_receipts r ON r.mission_id=h.mission_id "
        "WHERE NOT h.terminal OR h.uncertain ORDER BY h.mission_id");

    for (const auto& row : rows) {
        if (!row[4].is_null()) *snapshot.add_last_grants() = read(row[4]);

        if (!row[2].as<bool>()) {
            *snapshot.add_active_missions() = read(row[3]);
            continue;
        }

        auto* unknown = snapshot.add_unresolved();
        unknown->set_mission_id(row[0].as<std::string>());
        unknown->set_robot_id(row[1].as<std::string>());
        if (!row[3].is_null()) *unknown->mutable_last_status() = read(row[3]);
        if (!row[5].is_null()) unknown->set_add_request(decode(row[5]));
    }
}

}  // namespace execution_head

#endif //FLEETMANAGERSYSTEM_EXECUTIONHEAD_HPP
