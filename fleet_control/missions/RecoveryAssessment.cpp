#include "missions/RecoveryAssessment.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace recovery_assessment {
using namespace traffic;
using Inventory = fms::dispatch::v1::GetRecoveryInventoryResponse;

bool matches_move(const fms::v1::RobotStatus& status, const std::string& mission,
                  std::size_t step, const fms::v1::MoveProgress& progress) {
    namespace fms1 = fms::v1;
    const auto& order = status.observed_order();
    return status.connection() == fms1::CONNECTION_STATE_ONLINE && status.has_observed_order() &&
        !status.paused() && status.safety().emergency_stop() == fms1::EMERGENCY_STOP_NONE &&
        !status.safety().field_violation() && (status.operating_mode() == fms1::OPERATING_MODE_AUTOMATIC ||
        status.operating_mode() == fms1::OPERATING_MODE_SEMIAUTOMATIC) &&
        std::ranges::none_of(status.errors(), [](const auto& error) {
            return error.level() == fms1::ERROR_LEVEL_FATAL || error.level() == fms1::ERROR_LEVEL_CRITICAL;
        }) && status.mission_id() == mission && !order.idle() &&
        order.order_id() == mission + "-s" + std::to_string(step) &&
        order.update_id() == progress.confirmed_revision() && order.node_sequence() % 2 == 0 &&
        order.node_sequence() / 2 == progress.last_node_index() &&
        status.last_node_id() == progress.last_node_id();
}

GateResult assess(const Graph& graph, const Inventory& inventory,
    std::span<const TrafficOccupant> occupants, std::span<const ProtectedRoute> held,
    const Local& local, const ProtectedRoute& wanted, const std::string& extend_mission,
    bool candidate_ready, bool* inventory_mismatch) {
    if (inventory_mismatch) *inventory_mismatch = false;
    const auto mismatch = [&](const char* reason) -> GateResult {
        if (inventory_mismatch) *inventory_mismatch = true;
        return {false, reason};
    };
    if (inventory.state() != Inventory::READY || inventory.instance_id().empty() ||
        !inventory.revision() || !candidate_ready || wanted.nodes.empty() ||
        wanted.map != graph.map_id || wanted.version != graph.version)
        return {false, "inventory or candidate unavailable"};

    std::map<std::string, const TrafficOccupant*> fleet;
    for (const auto& o : occupants)
        if (o.robot.empty() || o.map.empty() || !fleet.emplace(o.robot, &o).second)
            return {false, "invalid control fleet"};
    std::set<std::string> registered(inventory.robots().begin(), inventory.robots().end());
    if (registered.size() != static_cast<std::size_t>(inventory.robots_size()) ||
        registered.size() != fleet.size() || !registered.contains(wanted.robot))
        return mismatch("fleet membership mismatch");
    for (const auto& id : registered)
        if (!fleet.contains(id)) return mismatch("fleet membership mismatch");

    std::set<std::string> blocked(inventory.blocked_robots().begin(), inventory.blocked_robots().end());
    const auto& restrictions = inventory.live_restrictions().robots();
    const std::set<std::string> restricted(restrictions.begin(), restrictions.end());
    for (const auto& robot : restricted)
        if (!blocked.contains(robot)) return {false, "inconsistent live restriction"};
    std::map<std::string, const fms::dispatch::v1::RecoveryMission*> rows;
    for (const auto& row : inventory.missions()) {
        const auto it = local.find(row.mission_id());
        if (it == local.end() || row.mission_id().empty() || !row.has_add_attempt() ||
            !blocked.contains(row.robot_id()) || it->second.robot != row.robot_id() ||
            it->second.attempt != row.add_attempt() || !rows.emplace(row.mission_id(), &row).second)
            return mismatch("unmatched dispatch mission");
    }
    std::map<std::string, const ProtectedRoute*> installed;
    for (const auto& route : held)
        if (!installed.emplace(route.robot, &route).second)
            return {false, "duplicate protected owner"};

    RecoveryBounds bounds{graph.map_id, graph.version, true, {}};
    std::set<std::string> owners;
    bool own_step = false;
    for (const auto& [id, proof] : local) {
        const auto found = installed.find(proof.robot);
        if (id.empty() || !registered.contains(proof.robot) || !owners.insert(proof.robot).second ||
            found == installed.end() || proof.steps.empty())
            return {false, "missing local authority or reservation"};
        const auto& route = *found->second;
        if (route.map != fleet.at(proof.robot)->map || route.map != graph.map_id ||
            route.version != graph.version)
            return {false, "cannot verify reserved map"};
        const auto remote = rows.find(id);
        if (!proof.recovering && (remote == rows.end() || remote->second->uncertain()))
            return mismatch("live mission requires reconciliation");
        const auto prefix = id + "/" + std::to_string(proof.attempt) + "/s";
        for (const auto& step : proof.steps)
            if (step.robot != proof.robot || step.map != route.map || step.version != route.version ||
                !step.key.starts_with(prefix) || step.key.size() == prefix.size())
                return {false, "mixed route authority"};
        const auto current = std::ranges::find(proof.steps, route.key, &ProtectedRoute::key);
        if (current == proof.steps.end() || current->nodes.empty() ||
            current->edges.size() != current->nodes.size() - 1 ||
            current->nodes.size() > route.nodes.size() || current->edges.size() > route.edges.size() ||
            !std::equal(current->nodes.begin(), current->nodes.end(), route.nodes.begin()) ||
            !std::equal(current->edges.begin(), current->edges.end(), route.edges.begin()))
            return {false, "current protected step mismatch"};
        if (proof.recovering || !inventory.has_live_restrictions() || restricted.contains(proof.robot))
            bounds.missions.push_back({proof.steps});
        if (id == extend_mission && !proof.recovering &&
            proof.robot == wanted.robot && route.key == wanted.key) own_step = true;
    }
    if (owners.size() != installed.size()) return {false, "orphan reservation"};
    for (const auto& robot : blocked)
        if (!owners.contains(robot)) return mismatch("unexplained blocked robot");
    for (const auto& [id, o] : fleet)
        if (!owners.contains(id) && (o->has_mission || !o->stationary))
            return {false, "unknown robot position or mission"};

    const auto& target = *fleet.at(wanted.robot);
    std::vector<ProtectedRoute> simulated(held.begin(), held.end());
    if (extend_mission.empty()) {
        if (owners.contains(wanted.robot) || blocked.contains(wanted.robot) || target.has_mission ||
            !target.stationary || target.map != graph.map_id || wanted.reached != 0 ||
            graph.find_node(target.node) != wanted.nodes.front())
            return {false, "new mission has no free origin"};
        simulated.push_back({wanted.robot, wanted.key, wanted.map, wanted.version,
                             {wanted.nodes.front()}, {}, 0});
    } else {
        if (!own_step || !target.has_mission || !inventory.has_live_restrictions())
            return {false, "extension lacks a verified running owner"};
        if (restricted.contains(wanted.robot))
            return {false, "extension owner unavailable"};
    }
    try {
        return FixedRouteGate{}.check(graph, occupants, simulated, wanted, bounds);
    } catch (const std::logic_error&) {
        return {false, "invalid route geometry or protected prefix"};
    }
}
} // namespace recovery_assessment
