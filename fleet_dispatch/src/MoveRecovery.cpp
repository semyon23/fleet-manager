#include "../include/MoveRecovery.hpp"
#include "../include/OrderBuilder.hpp"

#include <algorithm>
#include <unordered_map>
#include <google/protobuf/util/message_differencer.h>

namespace move_recovery {
namespace {
using namespace fms::v1;
using namespace vda5050;
using Execution = fms::database::v1::MissionExecution;
using Equal = google::protobuf::util::MessageDifferencer;

Result conflict(std::string_view reason) {
    return {Decision::Conflict, reason, {}};
}

Result wait(std::string_view reason) {
    return {Decision::Wait, reason, {}};
}

int move_index(const Execution& e) {
    if (!e.checkpoint_version() || !e.cancel_action_id().empty() ||
        e.status().state() != MISSION_STATE_RUNNING ||
        e.mission().robot_id() != e.status().robot_id() ||
        e.mission().mission_id() != e.status().mission_id() ||
        e.status().current_node_id().empty() ||
        e.mission().nodes_size() != e.status().steps_size()) {
        return -1;
    }

    for (int i = 0; i < e.mission().nodes_size(); ++i) {
        const auto& node = e.mission().nodes(i);
        const auto& step = e.status().steps(i);
        if (node.id() == e.status().current_node_id() && node.has_move() &&
            step.node_id() == node.id() && step.state() == STEP_STATE_RUNNING &&
            step.has_move() && step.attempts() > 0) {
            return i;
        }
    }
    return -1;
}

bool actions_match(const Order& full, const StepStatus& saved, const State& state) {
    struct Expected {
        const vda5050::Action* action;
        std::uint32_t sequence;
        bool released;
    };

    std::unordered_map<std::string, Expected> expected;
    auto collect = [&](const auto& elements) {
        for (const auto& element : elements) {
            for (const auto& a : element.actions) {
                if (a.action_id.empty() ||
                    !expected.emplace(a.action_id,
                        Expected{&a, element.sequence_id, element.released}).second) {
                    return false;
                }
            }
        }
        return true;
    };

    if (!collect(full.nodes) || !collect(full.edges) ||
        expected.size() != state.action_states.size() ||
        expected.size() != static_cast<std::size_t>(saved.actions_size())) {
        return false;
    }

    std::unordered_map<std::string, vda5050::ActionStatus> observed;
    for (const auto& a : state.action_states) {
        const auto it = expected.find(a.action_id);
        if (it == expected.end() || !observed.emplace(a.action_id, a.action_status).second)
            return false;

        const auto& x = it->second;
        if (a.action_type && *a.action_type != x.action->action_type) return false;
        if ((!x.released || x.sequence > state.last_node_sequence_id + 1u) &&
            a.action_status != vda5050::ActionStatus::Waiting) return false;
    }

    for (const auto& a : saved.actions()) {
        const auto it = observed.find(a.action_id());
        if (it == observed.end() ||
            a.action_type() != expected.at(a.action_id()).action->action_type) return false;

        if ((a.status() == ACTION_STATUS_FINISHED &&
             it->second != vda5050::ActionStatus::Finished) ||
            (a.status() == ACTION_STATUS_FAILED &&
             it->second != vda5050::ActionStatus::Failed)) return false;

        observed.erase(it);
    }
    return true;
}

}  // namespace

Result reconcile(const fms::database::v1::RobotSpec& robot, const Execution& head,
                 const Execution& grant, std::string_view request_id, const State* state) {
    const int i = move_index(head);
    if (i < 0 || move_index(grant) != i || !grant.has_order() ||
        robot.robot_id().empty() || robot.manufacturer().empty() || robot.serial_number().empty() ||
        head.mission().mission_id().empty() || head.mission().robot_id() != robot.robot_id() ||
        head.mission().mission_id() != grant.mission().mission_id() ||
        grant.mission().robot_id() != robot.robot_id() ||
        head.checkpoint_version() < grant.checkpoint_version()) {
        return conflict("checkpoint");
    }

    if (head.checkpoint_version() == grant.checkpoint_version() && !Equal::Equals(head, grant))
        return conflict("checkpoint mismatch");

    const auto& step = head.status().steps(i);
    const auto& previous = grant.status().steps(i);
    const auto& move = head.mission().nodes(i).move();
    const auto& saved = grant.order();
    const auto released = move.has_released_nodes() ? move.released_nodes() : move.nodes_size();

    auto normalized = grant.mission().nodes(i).move();
    if (!normalized.has_released_nodes()) normalized.set_released_nodes(normalized.nodes_size());
    auto current = move;
    current.set_released_nodes(released);

    auto original = grant.mission();
    *original.mutable_nodes(i)->mutable_move() = move;
    if (!Equal::Equals(original, head.mission())) return conflict("mission");

    std::string order_id = head.mission().mission_id() + "-" + step.node_id();
    if (step.attempts() > 1) order_id += "-a" + std::to_string(step.attempts());

    if (saved.order_id() != order_id || previous.node_id() != step.node_id() ||
        saved.mission_node_id() != step.node_id() || previous.attempts() != step.attempts() ||
        (head.has_order() && !Equal::Equals(head.order(), saved)) ||
        !Equal::Equals(current, normalized) || !validate_move_step(move).empty() ||
        step.move().revision() < previous.move().revision() ||
        saved.order_update_id() > previous.move().revision() ||
        step.move().released_nodes() != released || previous.move().released_nodes() != released ||
        step.move().total_nodes() != static_cast<unsigned>(move.nodes_size()) ||
        previous.move().total_nodes() != step.move().total_nodes()) {
        return conflict("grant");
    }

    for (const auto* progress : {&previous.move(), &step.move()}) {
        if (progress->last_node_index() >= released ||
            progress->last_node_id() != move.nodes(progress->last_node_index()).node_id())
            return conflict("checkpoint position");
    }

    if (!state || request_id.empty()) return wait("stateRequest");
    if (state->header.manufacturer != robot.manufacturer() ||
        state->header.serial_number != robot.serial_number()) return conflict("robot");

    const auto ack = [&](const auto& a) {
        return a.action_id == request_id &&
               (!a.action_type || *a.action_type == "stateRequest") &&
               a.action_status == vda5050::ActionStatus::Finished;
    };
    if (std::count_if(state->instant_action_states.begin(), state->instant_action_states.end(),
            [&](const auto& a) { return a.action_id == request_id; }) > 1)
        return conflict("duplicate response");

    if (std::none_of(state->instant_action_states.begin(), state->instant_action_states.end(), ack))
        return wait("stateRequest");

    if (state->order_id != order_id) return conflict("orderId");
    if (state->order_update_id > saved.order_update_id()) return conflict("newer update");

    const auto index = state->last_node_sequence_id / 2;
    if (state->last_node_sequence_id % 2 || index >= released ||
        move.nodes(index).node_id() != state->last_node_id ||
        index < step.move().last_node_index() || index < previous.move().last_node_index() ||
        (state->driving && index + 1 >= released)) {
        return conflict("position");
    }

    const auto full = build_move_order(order_id, move);
    auto observed_base = released;
    const bool older = state->order_update_id < saved.order_update_id();

    try {
        const auto payload = nlohmann::json::parse(saved.sent_order_json());
        const auto& nodes = payload.at("nodes");
        if (!nodes.is_array() || nodes.empty() || nodes.size() > full.nodes.size() ||
            payload.at("orderId") != order_id ||
            payload.at("orderUpdateId") != saved.order_update_id() ||
            payload.at("manufacturer") != robot.manufacturer() ||
            payload.at("serialNumber") != robot.serial_number()) return conflict("payload");

        const auto first = full.nodes.size() - nodes.size();
        if ((!saved.order_update_id() && first) || first >= released ||
            (saved.order_update_id() && first + 1 >= released)) return conflict("stitch");

        const auto& edges = payload.at("edges");
        if (!edges.is_array() || edges.size() + 1 != nodes.size()) return conflict("payload edges");
        for (std::size_t j = 0; j < nodes.size(); ++j) {
            if (nodes[j] != nlohmann::json(full.nodes[first + j])) return conflict("payload node");
            if (j < edges.size() && edges[j] != nlohmann::json(full.edges[first + j]))
                return conflict("payload edge");
        }

        if (older) {
            // Точка стыка последнего update — конец предыдущей base.
            if (index > first) return conflict("old base");
            observed_base = index + 1;
            for (const auto& n : state->node_states) {
                if (!n.released) break;
                ++observed_base;
            }
            if (observed_base > first + 1 ||
                (state->order_update_id + 1ULL == saved.order_update_id() &&
                 observed_base != first + 1) ||
                (state->driving && index + 1 >= observed_base)) return conflict("old base");
        }
    } catch (const nlohmann::json::exception&) {
        return conflict("invalid order JSON");
    }

    const auto remaining = full.nodes.size() - index - 1;
    if (state->node_states.size() != remaining || state->edge_states.size() != remaining)
        return conflict("remaining route");

    for (std::size_t j = 0; j < remaining; ++j) {
        const auto& n = state->node_states[j];
        const auto& e = state->edge_states[j];
        const auto& want_n = full.nodes[index + j + 1];
        const auto& want_e = full.edges[index + j];
        const bool allowed = index + j + 1 < observed_base;
        if (n.sequence_id != want_n.sequence_id || n.node_id != want_n.node_id ||
            n.released != allowed || e.sequence_id != want_e.sequence_id ||
            e.edge_id != want_e.edge_id || e.released != allowed)
            return conflict("remaining route");
    }

    if (older) return wait("older update");
    if (!actions_match(full, step, *state)) return conflict("actions");
    return {Decision::Attach, "matched", index};
}

    bool matches_attached_move(const vda5050::Order& full,
                           const fms::v1::StepStatus& step,
                           const vda5050::State& state) {
    const auto& progress = step.move();
    if (state.order_id != full.order_id ||
        state.order_update_id != full.order_update_id ||
        state.last_node_sequence_id % 2 != 0) {
        return false;
        }

    const auto index = state.last_node_sequence_id / 2;
    if (index >= full.nodes.size() ||
        index >= progress.released_nodes() ||
        index < progress.last_node_index() ||
        full.nodes[index].node_id != state.last_node_id ||
        (state.driving && index + 1 >= progress.released_nodes())) {
        return false;
        }

    const auto remaining = full.nodes.size() - index - 1;
    if (state.node_states.size() != remaining ||
        state.edge_states.size() != remaining) {
        return false;
        }

    for (std::size_t j = 0; j < remaining; ++j) {
        const auto& node = state.node_states[j];
        const auto& edge = state.edge_states[j];
        const auto& expected_node = full.nodes[index + j + 1];
        const auto& expected_edge = full.edges[index + j];

        if (node.node_id != expected_node.node_id ||
            node.sequence_id != expected_node.sequence_id ||
            node.released != expected_node.released ||
            edge.edge_id != expected_edge.edge_id ||
            edge.sequence_id != expected_edge.sequence_id ||
            edge.released != expected_edge.released) {
            return false;
            }
    }

    // Состав действий полного order уже проверен при restore_move().
    if (step.actions_size() == 0) return state.action_states.empty();
    return actions_match(full, step, state);
}

}  // namespace move_recovery