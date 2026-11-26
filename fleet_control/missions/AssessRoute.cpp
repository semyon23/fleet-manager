#include "missions/MissionRunner.hpp"

#include <algorithm>
#include <limits>

MissionRunner::Assessment MissionRunner::assess_route(const std::string& id, const std::string& robot) {
    return assess_motion(id, robot, false, nullptr);
}

MissionRunner::Assessment MissionRunner::assess_motion(const std::string& id, const std::string& robot,
    bool pending_add, const fms::dispatch::v1::ExtendMissionRequest* pending_extend) {
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
    const auto plan_it = plans_.find(id);
    const auto held_it = traffic_routes_.find(robot);
    const auto use_plan = [&](const Plan& plan, std::size_t step) {
        if (plan.traffic_version != graph->version || step >= plan.routes.size() || step >= plan.edges.size())
            return false;
        for (const auto& name : plan.routes[step]) {
            const auto node = graph->find_node(name);
            if (!node) return false;
            wanted.nodes.push_back(*node);
        }
        for (const auto& name : plan.edges[step]) {
            const auto edge = graph->edge_index.find(name);
            if (edge == graph->edge_index.end()) return false;
            wanted.edges.push_back(edge->second);
        }
        return !wanted.nodes.empty() && wanted.edges.size() == wanted.nodes.size() - 1;
    };
    const auto* observed = traffic_snapshot_ ? nullptr : scoped_progress(*mission);
    if (pending_add || mission->state == "QUEUED") {
        const auto idle = traffic_availability_->idle_robots();
        if (pending_extend || std::ranges::find(idle, robot, &AvailabilityTracker::IdleRobot::robot_id) == idle.end())
            return deny("assignment requires an available idle robot");
        if (pending_add) {
            if (!inventory->has_live_restrictions() || !sending_.contains(id) || !local_adds_.contains(id) ||
                add_rpc_started_.contains(id) || checkpoint_jobs_.contains(id) || uncertain_.contains(id) ||
                mission->state != "ASSIGNED" || mission->robot_id != robot || !command_journal_.all_stored(id) ||
                plan_it == plans_.end() || !use_plan(plan_it->second, 0))
                return deny("pending Add no longer owns its original route");
            for (const auto* other : missions_.active())
                if (other->id != id && other->robot_id == robot)
                    return deny("another mission owns this robot");
        } else if (!mission->robot_id.empty() || missions_.busy_robots().contains(robot)) {
            return deny("assignment requires an unassigned robot");
        }
        if (!pending_add) {
            const auto start = graph->find_node(status->last_node_id());
            const auto target = graph->find_node(mission->steps.front().node_id);
            auto* router = maps_.router(mission->map_id);
            if (!router || !start || !target) return deny("first step is outside the map");
            auto path = router->route(*start, graph->arrival_nodes(*target));
            if (!path) return deny("no route to first step");
            wanted.nodes = std::move(path->nodes);
            wanted.edges = std::move(path->edges);
        }
        wanted.key = key + "0";
    } else {
        const bool streamed = traffic_snapshot_ && !checkpoint_jobs_.contains(id) &&
                              mission_stream_instance_ == inventory->instance_id();
        if ((!streamed && !observed) || mission->state != "RUNNING" || mission->robot_id != robot ||
            !mission->current_step || *mission->current_step < 0 || uncertain_.contains(id) ||
            extensions_.contains(id) != (pending_extend != nullptr) || plan_it == plans_.end() ||
            held_it == traffic_routes_.end())
            return deny("no running step ready for extension");
        const auto& plan = plan_it->second;
        const auto step = static_cast<std::size_t>(*mission->current_step);
        if (!use_plan(plan, step) || step >= mission->step_states.size() ||
            mission->step_states[step].state != "RUNNING" || step >= plan.progress.size() ||
            (observed ? (observed->step != *mission->current_step || mission->steps.size() != 1 ||
                         !mission->steps.front().actions.empty()) :
                        (step >= plan.step_attempts.size() || plan.step_attempts[step] != 1)))
            return deny("running step or attempt is unverified");

        const auto& p = observed ? observed->progress : plan.progress[step];
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
        const auto& held = held_it->second;
        if (held.key != wanted.key || held.reached != wanted.reached ||
            !recovery_assessment::matches_move(*status, id, step, p))
            return deny("current robot order does not confirm the protected base");
        if (pending_extend) {
            const auto& entry = extensions_.at(id);
            if (entry.state() != ExtensionCommand::State::DispatchPending ||
                entry.request().SerializeAsString() != pending_extend->SerializeAsString() ||
                pending_extend->mission_id() != id || pending_extend->move_node_id() != "s" + std::to_string(step) ||
                pending_extend->revision() != p.revision() + 1 || !pending_extend->has_released_nodes() ||
                pending_extend->released_nodes() != p.total_nodes() || pending_extend->has_replace_tail() ||
                held.nodes != wanted.nodes || held.edges != wanted.edges)
                return deny("reserved extension differs from its command");
        } else if (held.nodes.size() != p.released_nodes()) {
            return deny("protected base length changed");
        }
        if (observed) {
            const auto parts = split_route(nodes, edges, p);
            if (!parts || parts->released.nodes != observed->parts.released.nodes ||
                parts->released.edges != observed->parts.released.edges ||
                parts->planned.nodes != observed->parts.planned.nodes ||
                parts->planned.edges != observed->parts.planned.edges)
                return deny("planned route differs from verified robot order");
        }
        extend = id;
    }

    auto local = assessment_evidence(pending_add ? id : std::string{});
    if (!local) return deny("local command authority incomplete");
    if (observed && !extend.empty()) {
        const auto it = local->find(id);
        if (it == local->end() || it->second.robot != robot || it->second.attempt != observed->add_attempt ||
            it->second.steps.size() != 1 || it->second.steps.front().key != wanted.key)
            return deny("target checkpoint does not cover this step");
        it->second.recovering = false; // This copy only. Other recovered owners keep all their bounds.
    }
    std::vector<traffic::ProtectedRoute> held;
    held.reserve(traffic_routes_.size());
    for (const auto& [owner, route] : traffic_routes_) held.push_back(route);
    const auto occupants = traffic_availability_->traffic_occupants();
    bool mismatch = false;
    auto result = recovery_assessment::assess(*graph, *inventory, occupants, held, *local,
                                              wanted, extend, true, &mismatch, pending_add);
    if (mismatch) wake_inventory_if_idle();
    if (!current_inventory()) return {{false, "dispatch inventory expired"}, {}, 0};
    return {std::move(result), inventory->instance_id(), inventory->revision(), std::move(wanted)};
}
