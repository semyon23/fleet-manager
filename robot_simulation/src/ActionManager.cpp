#include "../include/ActionManager.hpp"
#include "spdlog/spdlog.h"


std::optional<std::string> string_param(const vda5050::Action& a, const std::string& key) {
    if (!a.action_parameters) return std::nullopt;
    for (const auto& p : *a.action_parameters) {
        if (p.key == key && p.value.is_string()) return p.value.get<std::string>();
    }
    return std::nullopt;
}

std::optional<double> number_param(const vda5050::Action& a, const std::string& key) {
    if (!a.action_parameters) return std::nullopt;
    for (const auto& p : *a.action_parameters) {
        if (p.key == key && p.value.is_number()) return p.value.get<double>();
    }
    return std::nullopt;
}

bool is_terminal(vda5050::ActionStatus s) {
    return s == vda5050::ActionStatus::Finished || s == vda5050::ActionStatus::Failed;
}

ActionManager::ActionManager(vda5050::State& State) : State_(State) {}

void ActionManager::handle_instant_actions(const std::string& json){
    vda5050::InstantActions instant_actions;
    try {
        instant_actions = vda5050::json::parse(json).get<vda5050::InstantActions>();
    } catch (const std::exception& e) {
        add_warning(vda5050::error_type::kValidationFailure,
                    std::string("Malformed instantActions: ") + e.what(), {});
        return;
    }

    for (const auto& action : instant_actions.actions) {
        const bool known = std::any_of(State_.instant_action_states.begin(),
                                       State_.instant_action_states.end(),
                                       [&](const auto& s) { return s.action_id == action.action_id; });
        if (known) {
            continue;
        }
        vda5050::ActionState st;
        st.action_id = action.action_id;
        st.action_type = action.action_type;
        st.action_status = vda5050::ActionStatus::Waiting;
        State_.instant_action_states.push_back(std::move(st));

        execute_instant_action(action);
    }
}

void ActionManager::execute_instant_action(const vda5050::Action& a){
    const auto& type = a.action_type;

    if (type == vda5050::action_type::kCancelOrder){
        cancel_order(a);
    } else if (type == vda5050::action_type::kStartPause){
        set_paused(a, true);
        set_instant_status(a.action_id, vda5050::ActionStatus::Finished);
    } else if (type == vda5050::action_type::kStopPause){
        set_paused(a, false);
        set_instant_status(a.action_id, vda5050::ActionStatus::Finished);
    } else if (type == vda5050::action_type::kStateRequest){
        set_instant_status(a.action_id, vda5050::ActionStatus::Finished);
    } else if (type == vda5050::action_type::kFactsheetRequest){
        //publisher_.publish_factsheet();
        set_instant_status(a.action_id, vda5050::ActionStatus::Finished);
    } else if (type == vda5050::action_type::kInitializePosition){
        initialize_position(a);
        set_instant_status(a.action_id, vda5050::ActionStatus::Finished);
    } else if (actions_.supported_types().count(type)){
        actions_.enqueue({a}, /*instant=*/true);
    } else{
        set_instant_status(a.action_id, vda5050::ActionStatus::Failed);
        add_warning(vda5050::error_type::kInvalidInstantAction, "Unsupported instant action " + type,
                    {{"actionId", a.action_id}, {"actionType", type}});
    }
}


void ActionManager::add_warning(const std::string& type, const std::string& description, std::vector<vda5050::ErrorReference> refs){
    auto& errs = State_.errors;
    errs.erase(std::remove_if(errs.begin(), errs.end(),
                              [&](const vda5050::Error& e) { return e.error_type == type; }),
               errs.end());
    vda5050::Error e;
    e.error_type = type;
    e.error_level = vda5050::ErrorLevel::Warning;
    e.error_description = description;
    if (!refs.empty()) {
        e.error_references = std::move(refs);
    }
    errs.push_back(std::move(e));
    spdlog::error("[action_manager] type {}, description ({})", type, description);
}

void ActionManager::set_paused(const vda5050::Action& a, bool paused){
    State_.paused = paused;
    if (paused) {
        actions_.pause_all();     // pauseAllowed-action'ы → PAUSED; движение остановит HoldPosition
    } else {
        actions_.resume_all();    // дерево продолжит с текущей позиции
    }
}

void ActionManager::set_instant_status(const std::string& action_id, vda5050::ActionStatus status){
    for (auto& s : State_.instant_action_states) {
        if (s.action_id == action_id) {
            s.action_status = status;
            return;
        }
    }
}

void ActionManager::cancel_order(const vda5050::Action& a){
    const auto order_id = string_param(a, "orderId");
    const bool nothing_to_cancel =
        vda5050::is_idle(State_) || (order_id && *order_id != State_.order_id);
    if (nothing_to_cancel) {
        set_instant_status(a.action_id, vda5050::ActionStatus::Failed);
        add_warning(vda5050::error_type::kNoOrderToCancel, "No active order to cancel",
                    {{"actionId", a.action_id}});
        return;
    }

    stop_driving();
    actions_.cancel_all_order_actions();
    State_.node_states.clear();
    State_.edge_states.clear();
    State_.new_base_request = false;
    order_canceled_ = true;
    set_instant_status(a.action_id, vda5050::ActionStatus::Running);
    pending_cancel_id_ = a.action_id;
    update_pending_cancel();
}

void ActionManager::initialize_position(const vda5050::Action& a){
    const auto x = number_param(a, "x");
    const auto y = number_param(a, "y");
    const auto theta = number_param(a, "theta");
    const auto map_id = string_param(a, "mapId");
    const auto last_node_id = string_param(a, "lastNodeId");

    auto fail = [&](const std::string& why) {
        set_instant_status(a.action_id, vda5050::ActionStatus::Failed);
        add_warning(vda5050::error_type::kInvalidInstantAction, why, {{"actionId", a.action_id}});
    };
    if (!x || !y || !theta || !map_id || !last_node_id) {
        return fail("initializePosition: x, y, theta, mapId, lastNodeId are required");
    }
    if (!vda5050::is_idle(State_)) {
        return fail("initializePosition: robot has an active order");
    }
    if (State_.maps) {
        const bool enabled = std::any_of(State_.maps->begin(), State_.maps->end(), [&](const auto& m) {
            return m.map_id == *map_id && m.map_status == vda5050::EnableStatus::Enabled;
        });
        if (!enabled) {
            return fail("initializePosition: map " + *map_id + " is not enabled");
        }
    }

    auto& pos = State_.mobile_robot_position.emplace(
        State_.mobile_robot_position.value_or(vda5050::MobileRobotPosition{}));
    pos.x = *x;
    pos.y = *y;
    pos.theta = *theta;
    pos.map_id = *map_id;
    pos.localized = true;
    State_.last_node_id = *last_node_id;
    State_.last_node_sequence_id = 0;
    driver_.set_pose(*x, *y, *theta);
}

void ActionManager::update_pending_cancel(){
    if (!pending_cancel_id_) {
        return;
    }
    const bool all_done = std::all_of(State_.action_states.begin(), State_.action_states.end(),
                                      [](const auto& s) { return is_terminal(s.action_status); });
    if (all_done) {
        set_instant_status(*pending_cancel_id_, vda5050::ActionStatus::Finished);
        pending_cancel_id_.reset();

    }
}
