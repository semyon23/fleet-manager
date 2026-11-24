#pragma once

#include "missions/ExecutionRecovery.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

namespace recovery_test {
namespace db = fms::database::v1;
namespace dp = fms::dispatch::v1;
namespace t = fms::v1;
using namespace execution_recovery;
using nlohmann::json;

// The loop B–C–B gives two distinct positions with the same node ID.
struct Trip {
    Graph graph;
    Mission mission;
    dp::AddMissionRequest add;
    std::vector<Command> journal;
    Checkpoint cp;
    t::RobotStatus robot;
    json snapshot;
    std::vector<std::uint32_t> nodes, edges;

    explicit Trip(bool loop = false) {
        graph.map_id = "map";
        graph.version = 7;
        graph.nodes = {{"B", 0, 0}, {"C", 8, 0}};
        graph.node_index = {{"B", 0}, {"C", 1}};
        graph.edges = {{"BC", 0, 1, 8}, {"CB", 1, 0, 8}};
        graph.edge_index = {{"BC", 0}, {"CB", 1}};
        nodes = loop ? std::vector<std::uint32_t>{0, 1, 0} : std::vector<std::uint32_t>{0, 1};
        edges = loop ? std::vector<std::uint32_t>{0, 1} : std::vector<std::uint32_t>{0};
        const auto& goal = graph.nodes[nodes.back()];
        mission.id = "trip";
        mission.robot_id = "r";
        mission.map_id = graph.map_id;
        mission.steps.push_back({goal.id});
        auto* m = add.mutable_mission();
        m->set_mission_id(mission.id);
        m->set_robot_id(mission.robot_id);
        auto* s = m->add_nodes();
        s->set_id("s0");
        s->set_name(goal.id);
        auto* move = s->mutable_move();
        move->set_released_nodes(1);
        std::vector<std::string> path, links;
        for (const auto index : nodes) {
            const auto& n = graph.nodes[index];
            auto* item = move->add_nodes();
            item->set_node_id(n.id);
            item->set_map_id(graph.map_id);
            item->set_x(n.x);
            path.push_back(n.id);
        }
        for (const auto index : edges) {
            links.push_back(graph.edges[index].id);
            move->add_edges()->set_edge_id(links.back());
        }
        snapshot = {{"version", 1}, {"attempt", 0}, {"mapVersion", 7},
                    {"routes", json::array({path})}, {"edges", json::array({links})},
                    {"protectedBase", {{"robot", "r"}, {"key", "trip/0/s0"}, {"map", "map"},
                        {"version", 7}, {"nodes", {0}}, {"edges", json::array()}, {"reached", 0}}}};
        snapshot["mission"] = mission_json(mission);
        record(db::CONTROL_COMMAND_KIND_ADD_MISSION, add.SerializeAsString());
        snapshot["protectedBase"]["nodes"] = nodes;
        snapshot["protectedBase"]["edges"] = edges;
        dp::ExtendMissionRequest ext;
        ext.set_mission_id("trip");
        ext.set_move_node_id("s0");
        ext.set_revision(1);
        ext.set_released_nodes(nodes.size());
        record(db::CONTROL_COMMAND_KIND_EXTEND_MISSION, ext.SerializeAsString());
        journal.back().set_move_node_id("s0");
        journal.back().set_revision(1);

        cp.set_kind(Checkpoint::CONFIRMED_TERMINAL);
        cp.set_checkpoint_version(6);
        cp.set_add_attempt(0);
        cp.set_robot_id("r");
        cp.set_mission_id("trip");
        cp.set_accepted_add(add.SerializeAsString());
        *cp.mutable_executed_mission() = *m;
        cp.mutable_executed_mission()->mutable_nodes(0)->mutable_move()->set_released_nodes(nodes.size());
        auto* status = cp.mutable_status();
        status->set_mission_id("trip");
        status->set_robot_id("r");
        status->set_state(t::MISSION_STATE_COMPLETED);
        auto* step = status->add_steps();
        step->set_node_id("s0");
        step->set_attempts(1);
        step->set_state(t::STEP_STATE_COMPLETED);
        auto* p = step->mutable_move();
        p->set_total_nodes(nodes.size());
        p->set_released_nodes(nodes.size());
        p->set_last_node_index(nodes.size() - 1);
        p->set_last_node_id(goal.id);
        p->set_revision(1);
        p->set_confirmed_revision(1);
        cp.set_grant_order("trip-s0");
        cp.set_grant_step("s0");
        cp.set_grant_update(1);
        robot.set_robot_id("r");
        robot.set_connection(t::CONNECTION_STATE_ONLINE);
        robot.set_localized(true);
        robot.set_last_node_id(goal.id);
        robot.mutable_pose()->set_map_id("map");
        robot.mutable_pose()->set_x(goal.x);
        auto* o = robot.mutable_observed_order();
        o->set_order_id(cp.grant_order());
        o->set_update_id(1);
        o->set_node_sequence(2 * (nodes.size() - 1));
        o->set_idle(true);
    }

    void record(db::ControlCommandKind kind, const std::string& bytes) {
        auto& c = journal.emplace_back();
        c.set_command_id(std::to_string(journal.size()));
        c.set_kind(kind);
        c.set_mission_id("trip");
        c.set_robot_id("r");
        c.set_map_id("map");
        c.set_map_version(7);
        c.set_recovery_version(1);
        c.set_request_proto(bytes);
        c.set_recovery_json(snapshot.dump());
    }

    void policy(t::FailureAction policy) {
        for (auto* mission : {add.mutable_mission(), cp.mutable_executed_mission()}) {
            auto* step = mission->mutable_nodes(0);
            step->set_on_failure(policy);
            if (policy == t::FAILURE_ACTION_RETRY) step->set_max_retries(1);
        }
        cp.set_accepted_add(add.SerializeAsString());
        journal[0].set_request_proto(add.SerializeAsString());
    }

    auto authority() const { return read_authority(mission, graph, journal, 0); }

    Decision decide() const {
        auto a = authority();
        REQUIRE(a.has_value());
        return terminal(*a, graph, cp);
    }
};
} // namespace recovery_test
