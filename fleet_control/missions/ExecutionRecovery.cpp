#include "missions/ExecutionRecovery.hpp"
#include "missions/RecoveryAssessment.hpp"
#include "fleet/Availability.hpp"
#include <algorithm>
#include <map>
#include <google/protobuf/util/message_differencer.h>

namespace execution_recovery {
namespace {
namespace db = fms::database::v1;
namespace dp = fms::dispatch::v1;
namespace fms1 = fms::v1;
using nlohmann::json;

bool snapshot_matches(const Command& cmd, const Grant& grant) {
    const auto snapshot = json::parse(cmd.recovery_json());
    const auto& base = snapshot.at("protectedBase");
    const auto& route = grant.base;
    return snapshot.at("version") == 1 && snapshot.at("attempt") == cmd.attempt() &&
           snapshot.at("mapVersion") == route.version && base.at("robot") == route.robot &&
           base.at("key") == route.key && base.at("map") == route.map &&
           base.at("version") == route.version && base.at("nodes") == route.nodes &&
           base.at("edges") == route.edges && base.at("reached").is_number_unsigned() &&
           base.at("reached").get<std::uint64_t>() < route.nodes.size();
}

bool set_prefix(Grant& grant, const fms1::MoveStep& move, const Graph& graph,
                std::uint32_t count) {
    if (!count || count < grant.base.nodes.size() || count > static_cast<unsigned>(move.nodes_size()))
        return false;
    for (auto i = grant.base.nodes.size(); i < count; ++i) {
        const auto& n = move.nodes(static_cast<int>(i));
        const auto v = graph.find_node(n.node_id());
        if (!v || n.map_id() != graph.map_id || n.x() != graph.nodes[*v].x || n.y() != graph.nodes[*v].y)
            return false;
        if (i) {
            const auto& eid = move.edges(static_cast<int>(i - 1)).edge_id();
            const auto pos = graph.edge_index.find(eid);
            if (pos == graph.edge_index.end() || pos->second >= graph.edges.size()) return false;
            const auto& edge = graph.edges[pos->second];
            if (edge.id != eid || edge.from != grant.base.nodes.back() || edge.to != *v) return false;
            grant.base.edges.push_back(pos->second);
        }
        grant.base.nodes.push_back(*v);
    }
    return true;
}

bool compatible_progress(const fms1::MoveProgress& p, const fms1::MoveStep& move,
                         const Grant& grant) {
    return p.total_nodes() == static_cast<unsigned>(move.nodes_size()) &&
           p.released_nodes() && p.released_nodes() <= grant.base.nodes.size() &&
           p.last_node_index() < p.released_nodes() &&
           p.last_node_id() == move.nodes(static_cast<int>(p.last_node_index())).node_id() &&
           p.confirmed_revision() <= p.revision() && p.revision() <= grant.revision;
}
} // namespace

std::optional<Authority> read_authority(const Mission& mission, const Graph& graph,
                                       std::span<const Command> records, std::uint32_t attempt) {
    Authority a;
    if (mission.map_id != graph.map_id || mission.robot_id.empty()) return {};
    const Command* add = nullptr;
    const Command* cancel = nullptr;
    std::map<std::string, std::map<std::uint32_t, const Command*>> extensions;
    for (const auto& cmd : records) {
        if (cmd.mission_id() != mission.id || cmd.attempt() > attempt) return {};
        if (cmd.attempt() < attempt) {
            // A different Add can win this mission only after an earlier Add was refused.
            if (cmd.kind() != db::CONTROL_COMMAND_KIND_ADD_MISSION) return {};
            continue;
        }
        if (cmd.robot_id() != mission.robot_id || cmd.map_id() != graph.map_id ||
            cmd.map_version() != graph.version || cmd.recovery_version() != 1) return {};
        switch (cmd.kind()) {
        case db::CONTROL_COMMAND_KIND_ADD_MISSION:
            if (add) return {};
            add = &cmd;
            break;
        case db::CONTROL_COMMAND_KIND_EXTEND_MISSION:
            if (!extensions[cmd.move_node_id()].emplace(cmd.revision(), &cmd).second) return {};
            break;
        case db::CONTROL_COMMAND_KIND_CANCEL_MISSION: {
            dp::CancelMissionRequest req;
            if (cancel || !req.ParseFromString(cmd.request_proto()) || req.mission_id() != mission.id)
                return {};
            cancel = &cmd;
            break;
        }
        default: return {};
        }
    }
    if (!add || !a.add.ParseFromString(add->request_proto()) || a.add.attempt() != attempt) return {};
    const auto& original = a.add.mission();
    if (original.mission_id() != mission.id || original.robot_id() != mission.robot_id ||
        original.nodes_size() == 0 || original.nodes_size() != static_cast<int>(mission.steps.size()))
        return {};
    try {
        const auto saved = json::parse(add->recovery_json());
        const auto& original_doc = saved.at("mission");
        const auto current_doc = mission_json(mission);
        for (const auto* field : {"id", "robotId", "mapId", "name", "priority", "requestedRobotId", "steps", "createdAt"}) {
            if (original_doc.at(field) != current_doc.at(field)) return {};
        }
        const auto& paths = saved.at("routes");
        const auto& links = saved.at("edges");
        if (!paths.is_array() || !links.is_array() || paths.size() != mission.steps.size() ||
            links.size() != paths.size()) return {};
        for (int i = 0; i < original.nodes_size(); ++i) {
            const auto& step = original.nodes(i);
            const std::string sid = "s" + std::to_string(i);
            const auto& move = step.move();
            if (step.id() != sid || !step.has_move() || !move.has_released_nodes() ||
                move.released_nodes() != 1 || move.nodes_size() < 1 ||
                move.edges_size() != move.nodes_size() - 1) return {};
            if (paths[i].size() != static_cast<std::size_t>(move.nodes_size()) ||
                links[i].size() != static_cast<std::size_t>(move.edges_size()) ||
                step.name() != mission.steps[i].node_id) return {};
            if (i && paths[i].front() != paths[i - 1].back()) return {};
            for (int j = 0; j < move.nodes_size(); ++j) {
                if (paths[i][j] != move.nodes(j).node_id() ||
                    (j && links[i][j - 1] != move.edges(j - 1).edge_id())) return {};
            }
            Grant geometry;
            if (!set_prefix(geometry, move, graph, move.nodes_size())) return {};
            const auto goal = graph.find_node(mission.steps[i].node_id);
            const auto dest = graph.find_node(move.nodes(move.nodes_size()-1).node_id());
            if (!goal || !dest) return {};
            const auto targets = graph.arrival_nodes(*goal);
            if (std::ranges::find(targets, *dest) == targets.end()) return {};
            auto& g = a.steps.emplace_back();
            g.base.robot = mission.robot_id;
            g.base.map = mission.map_id;
            g.base.version = graph.version;
            g.base.key = mission.id + "/" + std::to_string(attempt) + "/" + sid;
            if (!set_prefix(g, move, graph, 1) || (i == 0 && !snapshot_matches(*add, g))) return {};
            if (const auto it = extensions.find(sid); it != extensions.end()) {
                for (const auto& [rev, cmd] : it->second) {
                    dp::ExtendMissionRequest req;
                    if (rev != g.revision + 1 || !req.ParseFromString(cmd->request_proto()) ||
                        req.mission_id() != mission.id || req.move_node_id() != sid ||
                        req.revision() != rev || req.has_replace_tail() || !req.has_released_nodes() ||
                        req.released_nodes() <= g.base.nodes.size() ||
                        !set_prefix(g, move, graph, req.released_nodes()) || !snapshot_matches(*cmd, g))
                        return {};
                    g.revision = rev;
                    const auto reached = json::parse(cmd->recovery_json()).at("protectedBase").at("reached").get<size_t>();
                    if (reached < g.base.reached) return {};
                    g.base.reached = reached;
                }
                extensions.erase(it);
            }
        }
        if (!extensions.empty()) return {};
        if (cancel) {
            auto held = std::ranges::find_if(a.steps, [&](const Grant& g) { return snapshot_matches(*cancel, g); });
            if (held == a.steps.end()) return {};
            const auto reached = json::parse(cancel->recovery_json()).at("protectedBase").at("reached").get<std::size_t>();
            held->base.reached = std::max(held->base.reached, reached);
            a.cancel = true;
        }
    } catch (const json::exception&) {
        return {};
    }
    return a;
}

bool matches_execution(const Authority& a, const Checkpoint& cp) {
    using google::protobuf::util::MessageDifferencer;
    dp::AddMissionRequest receipt;
    if (cp.accepted_add().empty() || !receipt.ParseFromString(cp.accepted_add()) ||
        !MessageDifferencer::Equals(receipt, a.add) || !cp.has_executed_mission()) return false;
    auto effective = cp.executed_mission();
    if (effective.nodes_size() != static_cast<int>(a.steps.size()) ||
        cp.status().steps_size() != effective.nodes_size()) return false;
    for (int i = 0; i < effective.nodes_size(); ++i) {
        auto* node = effective.mutable_nodes(i);
        if (!node->has_move() || cp.status().steps(i).node_id() != node->id()) return false;
        auto* move = node->mutable_move();
        if (!move->has_released_nodes() || move->released_nodes() == 0 ||
            move->released_nodes() > a.steps[i].base.nodes.size()) return false;
        if (cp.status().steps(i).has_move() &&
            cp.status().steps(i).move().released_nodes() != move->released_nodes()) return false;
        move->set_released_nodes(a.add.mission().nodes(i).move().released_nodes());
    }
    return MessageDifferencer::Equals(effective, a.add.mission());
}

std::optional<ObservedMove> observe_move(const Checkpoint& cp, const dp::GetMissionRequest& request,
    const dp::GetMissionResponse& reply, const fms1::RobotStatus& robot, const ObservedMove* previous) {
    const auto& mission = reply.mission();
    const auto& state = reply.status();
    const auto& id = request.mission_id();
    if (id.empty() || request.expected_instance_id().empty() || request.expected_robot_id().empty() ||
        !request.has_expected_add_attempt() || !reply.has_add_attempt() || !cp.has_add_attempt() ||
        reply.instance_id() != request.expected_instance_id() ||
        reply.add_attempt() != request.expected_add_attempt() || cp.add_attempt() != reply.add_attempt() ||
        robot.robot_id() != request.expected_robot_id() || cp.robot_id() != robot.robot_id() ||
        mission.robot_id() != robot.robot_id() || state.robot_id() != robot.robot_id() ||
        cp.mission_id() != id || mission.mission_id() != id || state.mission_id() != id ||
        cp.kind() != Checkpoint::ACTIVE || !cp.has_grant_update() || !cp.has_status() ||
        cp.status().current_node_id() != cp.grant_step() || cp.status().steps_size() != mission.nodes_size() ||
        state.state() != fms1::MISSION_STATE_RUNNING || cp.grant_step() != state.current_node_id() ||
        !google::protobuf::util::MessageDifferencer::Equals(cp.executed_mission(), mission) ||
        mission.nodes_size() != state.steps_size()) return {};

    for (int i = 0; i < mission.nodes_size(); ++i) {
        const auto& node = mission.nodes(i);
        if (node.id() != state.current_node_id()) continue;
        const auto& step = state.steps(i);
        const auto& move = node.move();
        const auto& progress = step.move();
        if (node.id() != "s" + std::to_string(i) || !node.has_move() ||
            step.node_id() != node.id() || step.state() != fms1::STEP_STATE_RUNNING ||
            step.attempts() != 1 || !step.has_move() || !move.has_released_nodes() ||
            !move.nodes_size() || !robot.localized() || !robot.has_pose() ||
            move.nodes(0).map_id() != robot.pose().map_id() ||
            progress.released_nodes() != move.released_nodes() || progress.last_node_id().empty() ||
            progress.revision() != progress.confirmed_revision() || !progress.update_rejected().empty() ||
            cp.grant_order() != robot.observed_order().order_id() ||
            cp.grant_update() != progress.confirmed_revision() ||
            !recovery_assessment::matches_move(robot, id, i, progress)) return {};
        const auto& committed = cp.status().steps(i);
        if (committed.node_id() != step.node_id() || committed.attempts() > 1 ||
            (committed.has_move() && progress.last_node_index() < committed.move().last_node_index())) return {};
        if (previous && previous->add_attempt == reply.add_attempt() &&
            (i < previous->step || (i == previous->step &&
            (progress.last_node_index() < previous->progress.last_node_index() ||
             progress.released_nodes() < previous->progress.released_nodes() ||
             progress.revision() < previous->progress.revision())))) return {};

        std::vector<std::string> nodes, edges;
        for (const auto& n : move.nodes()) nodes.push_back(n.node_id());
        for (const auto& e : move.edges()) edges.push_back(e.edge_id());
        auto parts = split_route(nodes, edges, progress);
        if (parts) return ObservedMove{reply.add_attempt(), i, progress, std::move(*parts)};
        break;
    }
    return {};
}

Decision terminal(const Authority& a, const Graph& graph, const Checkpoint& cp) {
    const auto& mission = a.add.mission();
    const auto& s = cp.status();
    if (cp.kind() != Checkpoint::CONFIRMED_TERMINAL || !cp.has_add_attempt() ||
        !matches_execution(a, cp) ||
        cp.add_attempt() != a.add.attempt() || !cp.checkpoint_version() || !cp.has_status() ||
        cp.mission_id() != mission.mission_id() || s.mission_id() != cp.mission_id() ||
        cp.robot_id() != mission.robot_id() || s.robot_id() != cp.robot_id() ||
        !s.current_node_id().empty() || s.steps_size() != mission.nodes_size() ||
        a.steps.size() != static_cast<size_t>(s.steps_size()) || a.steps.empty()) return {};
    const bool completed = s.state() == fms1::MISSION_STATE_COMPLETED;
    if (!completed && (s.state() != fms1::MISSION_STATE_CANCELED || !a.cancel)) return {};

    int last = -1;
    bool whole_route = true;
    bool predecessors_clear = true;
    for (int i = 0; i < s.steps_size(); ++i) {
        const auto& st = s.steps(i);
        const auto& g = a.steps[i];
        const auto& n = mission.nodes(i);
        if (st.node_id() != n.id() || g.base.map != graph.map_id || g.base.version != graph.version)
            return {};
        if (st.attempts() == 0) {
            if (completed || st.has_move() || g.revision ||
                (st.state() != fms1::STEP_STATE_PENDING && st.state() != fms1::STEP_STATE_CANCELED))
                return {};
            continue;
        }
        if (last + 1 != i ||
            (st.attempts() > 1 && (n.on_failure() != fms1::FAILURE_ACTION_RETRY ||
                                   st.attempts() - 1 > n.max_retries())) ||
            (st.state() == fms1::STEP_STATE_SKIPPED && n.on_failure() != fms1::FAILURE_ACTION_SKIP)) return {};
        predecessors_clear = whole_route;
        last = i;
        if (st.state() != fms1::STEP_STATE_COMPLETED && st.state() != fms1::STEP_STATE_SKIPPED &&
            (completed || st.state() != fms1::STEP_STATE_CANCELED)) return {};
        if (!st.has_move()) {
            whole_route = false;
            continue;
        }
        const auto& p = st.move();
        if (!compatible_progress(p, n.move(), g)) return {};
        if (st.state() != fms1::STEP_STATE_COMPLETED || p.waiting_for_release() ||
            !std::ranges::all_of(st.actions(), [](const auto& act) { return act.status() == fms1::ACTION_STATUS_FINISHED; }) ||
            p.confirmed_revision() != g.revision ||
            p.revision() != g.revision || !p.update_rejected().empty() ||
            p.last_node_index() + 1 != p.total_nodes()) whole_route = false;
    }
    if (last < 0) return {};
    const auto& step = s.steps(last);
    if (!completed && step.state() != fms1::STEP_STATE_CANCELED) return {};
    const auto& grant = a.steps[last];
    Decision result{true, predecessors_clear, grant.base, std::nullopt};
    if (!step.has_move() || !predecessors_clear || (completed && !whole_route)) return result;
    const auto& p = step.move();
    auto order_id = mission.mission_id() + "-" + step.node_id();
    if (step.attempts() > 1) order_id += "-a" + std::to_string(step.attempts());
    if (p.last_node_index() < grant.base.reached || p.released_nodes() != grant.base.nodes.size() ||
        p.confirmed_revision() != grant.revision || !p.update_rejected().empty() ||
        !cp.has_grant_update() || cp.grant_order() != order_id || cp.grant_step() != step.node_id())
        return result;
    // A canceled robot near the departure node can already be physically on the edge.
    if (!completed && p.last_node_index() + 1 != p.released_nodes()) return result;
    result.expected_stop = Decision::Stop{p.last_node_id(), order_id, cp.grant_update(), p.last_node_index()};
    return result;
}

bool Decision::stopped_on_node(const Graph& graph, const fms1::RobotStatus* robot) const {
    if (!held || !expected_stop || !robot || robot->robot_id() != held->robot ||
        graph.map_id != held->map || graph.version != held->version || !robot->mission_id().empty() ||
        robot->connection() != fms1::CONNECTION_STATE_ONLINE || !robot->has_observed_order() ||
        !::stopped_on_node(*robot, graph, expected_stop->node)) return false;
    const auto& obs = robot->observed_order();
    return obs.idle() && obs.order_id() == expected_stop->order && obs.update_id() == expected_stop->update &&
           obs.node_sequence() == 2ULL * expected_stop->index;
}

} // namespace execution_recovery
