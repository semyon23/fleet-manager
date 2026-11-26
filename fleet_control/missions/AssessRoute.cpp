#include "missions/MissionRunner.hpp"

#include <algorithm>
#include <limits>

MissionRunner::Assessment MissionRunner::assess_route(const std::string& id, const std::string& robot) {
    const auto* inventory = current_inventory();
    if (!inventory) {
        wake_inventory_if_idle();
        return {{false, "dispatch inventory unavailable"}, {}, 0};
    }
    const auto deny = [&](const char* reason) -> Assessment {
        return {{false, reason}, inventory->instance_id(), inventory->revision()};
    };
    if (stopped_ || inventory->state() != Inventory::READY || traffic_fault_ ||
        !journal_loaded_ || !maps_.loaded() || !missions_.loaded() || !traffic_availability_ ||
        !traffic_availability_->assessment_fleet_ready() || !traffic_editing_.empty() ||
        traffic_availability_->traffic_instance() != inventory->instance_id())
        return deny("control or dispatch inventory incomplete");

    const auto* mission = missions_.find(id);
    if (!mission || mission->steps.empty() || robot.empty() ||
        (!mission->requested_robot_id.empty() && mission->requested_robot_id != robot))
        return deny("mission or requested robot unavailable");
    const auto* graph = maps_.find(mission->map_id);
    const auto* status = traffic_availability_->live_status(robot);
    if (!graph || !status || status->robot_id() != robot || !status->has_pose() ||
        !status->localized() || status->pose().map_id() != graph->map_id)
        return deny("robot lacks a current position on this map");

    traffic::ProtectedRoute wanted;
    wanted.robot = robot;
    wanted.map = graph->map_id;
    wanted.version = graph->version;
    const auto key = id + "/" + std::to_string(journal_attempt(id)) + "/s";
    std::string extend;
    if (mission->state == "QUEUED") {
        const auto idle = traffic_availability_->idle_robots();
        if (!mission->robot_id.empty() || missions_.busy_robots().contains(robot) ||
            std::ranges::find(idle, robot, &AvailabilityTracker::IdleRobot::robot_id) == idle.end())
            return deny("assignment requires an available idle robot");
        const auto start = graph->find_node(status->last_node_id());
        const auto target = graph->find_node(mission->steps.front().node_id);
        auto* router = maps_.router(mission->map_id);
        if (!router || !start || !target) return deny("first step is outside the map");
        auto path = router->route(*start, graph->arrival_nodes(*target));
        if (!path) return deny("no route to first step");
        wanted.key = key + "0";
        wanted.nodes = std::move(path->nodes);
        wanted.edges = std::move(path->edges);
    } else {
        const auto plan_it = plans_.find(id);
        const auto held = traffic_routes_.find(robot);
        if (!traffic_snapshot_ || mission_stream_instance_ != inventory->instance_id() ||
            mission->state != "RUNNING" || mission->robot_id != robot ||
            !mission->current_step || *mission->current_step < 0 || checkpoint_jobs_.contains(id) ||
            uncertain_.contains(id) || extensions_.contains(id) || plan_it == plans_.end() ||
            held == traffic_routes_.end())
            return deny("no running step ready for extension");
        const auto& plan = plan_it->second;
        const auto step = static_cast<std::size_t>(*mission->current_step);
        if (plan.traffic_version != graph->version || step >= mission->step_states.size() ||
            mission->step_states[step].state != "RUNNING" || step >= plan.progress.size() ||
            step >= plan.step_attempts.size() || plan.step_attempts[step] != 1 ||
            step >= plan.routes.size() || step >= plan.edges.size())
            return deny("running step or attempt is unverified");

        const auto& p = plan.progress[step];
        const auto& nodes = plan.routes[step];
        const auto& edges = plan.edges[step];
        if (nodes.empty() || edges.size() != nodes.size() - 1 || p.total_nodes() != nodes.size() ||
            !p.released_nodes() || p.released_nodes() >= p.total_nodes() ||
            p.last_node_index() >= p.released_nodes() || p.last_node_id() != nodes[p.last_node_index()] ||
            p.revision() != p.confirmed_revision() || !p.update_rejected().empty() ||
            p.revision() == std::numeric_limits<std::uint32_t>::max())
            return deny("step has no confirmed extendable base");
        wanted.key = key + std::to_string(step);
        wanted.reached = p.last_node_index();
        if (held->second.key != wanted.key || held->second.reached != wanted.reached ||
            held->second.nodes.size() != p.released_nodes() ||
            !recovery_assessment::matches_move(*status, id, step, p))
            return deny("current robot order does not confirm the protected base");

        for (const auto& name : nodes) {
            const auto node = graph->find_node(name);
            if (!node) return deny("planned node missing from map");
            wanted.nodes.push_back(*node);
        }
        for (const auto& name : edges) {
            const auto edge = graph->edge_index.find(name);
            if (edge == graph->edge_index.end()) return deny("planned edge missing from map");
            wanted.edges.push_back(edge->second);
        }
        extend = id;
    }

    const auto local = assessment_evidence();
    if (!local) return deny("local command authority incomplete");
    std::vector<traffic::ProtectedRoute> held;
    held.reserve(traffic_routes_.size());
    for (const auto& [owner, route] : traffic_routes_) held.push_back(route);
    const auto occupants = traffic_availability_->traffic_occupants();
    bool mismatch = false;
    auto result = recovery_assessment::assess(*graph, *inventory, occupants, held, *local,
                                              wanted, extend, true, &mismatch);
    if (mismatch) wake_inventory_if_idle();
    if (!current_inventory()) return {{false, "dispatch inventory expired"}, {}, 0};
    return {std::move(result), inventory->instance_id(), inventory->revision()};
}
