#include "include/vda/ActionManager.hpp"

#include <algorithm>
#include "spdlog/spdlog.h"

namespace {

namespace v = vda5050;

bool is_terminal(v::ActionStatus s) {
    return s == v::ActionStatus::Finished || s == v::ActionStatus::Failed;
}

std::optional<double> number_param(const v::Action& a, const std::string& key) {
    if (!a.action_parameters) return std::nullopt;
    for (const auto& p : *a.action_parameters) {
        if (p.key == key && p.value.is_number()) return p.value.get<double>();
    }
    return std::nullopt;
}

std::optional<std::string> string_param(const v::Action& a, const std::string& key) {
    if (!a.action_parameters) return std::nullopt;
    for (const auto& p : *a.action_parameters) {
        if (p.key == key && p.value.is_string()) return p.value.get<std::string>();
    }
    return std::nullopt;
}

}  // namespace

ActionManager::ActionManager(vda5050::State& state, ActionExecutor& executor, RobotDriveController& driver,
                             OrderManager& order_manager, StatePublisher& publisher)
    : State_(state), action_executor_(executor), driver_(driver),
      order_manager_(order_manager), publisher_(publisher) {}

// ---------------------------------------------------------------------------
// Вход
// ---------------------------------------------------------------------------

void ActionManager::handle_instant_actions(const std::string& json) {
    v::InstantActions msg;
    try {
        msg = v::json::parse(json).get<v::InstantActions>();
    } catch (const std::exception& e) {
        add_warning(v::error_type::kValidationFailure,
                    std::string("Malformed instantActions: ") + e.what(), {});
        publisher_.emit_state_changed();
        return;
    }

    for (const auto& action : msg.actions) {
        const bool known = std::any_of(State_.instant_action_states.begin(),
                                       State_.instant_action_states.end(),
                                       [&](const auto& s) { return s.action_id == action.action_id; });
        if (known) {
            continue;
        }
        v::ActionState st;
        st.action_id = action.action_id;
        st.action_type = action.action_type;
        st.action_status = v::ActionStatus::Waiting;
        State_.instant_action_states.push_back(std::move(st));

        execute_instant_action(action);
    }
    // Заодно отвечает на stateRequest: state изменился (новый instantActionState), уйдёт сразу.
    publisher_.emit_state_changed();
}

void ActionManager::execute_instant_action(const v::Action& a) {
    const auto& type = a.action_type;

    if (type == v::action_type::kCancelOrder) {
        cancel_order(a);
    } else if (type == v::action_type::kStartPause) {
        set_paused(a, true);
    } else if (type == v::action_type::kStopPause) {
        set_paused(a, false);
    } else if (type == v::action_type::kStateRequest) {
        set_instant_status(a.action_id, v::ActionStatus::Finished);
    } else if (type == v::action_type::kFactsheetRequest) {
        publisher_.publish_factsheet();
        set_instant_status(a.action_id, v::ActionStatus::Finished);
    } else if (type == v::action_type::kInitializePosition) {
        initialize_position(a);
    } else if (action_executor_.supported_types().count(type)) {
        // Свои instant action'ы — через исполнитель.
        action_executor_.enqueue({a}, /*instant=*/true);
    } else {
        set_instant_status(a.action_id, v::ActionStatus::Failed);
        add_warning(v::error_type::kInvalidInstantAction, "Unsupported instant action " + type,
                    {{"actionId", a.action_id}, {"actionType", type}});
    }
}

// ---------------------------------------------------------------------------
// cancelOrder (6.1.3)
// ---------------------------------------------------------------------------

void ActionManager::cancel_order(const v::Action& a) {
    const auto order_id = string_param(a, "orderId");
    const bool nothing_to_cancel =
        v::is_idle(State_) || (order_id && *order_id != State_.order_id);
    if (nothing_to_cancel) {
        set_instant_status(a.action_id, v::ActionStatus::Failed);
        add_warning(v::error_type::kNoOrderToCancel, "No active order to cancel",
                    {{"actionId", a.action_id}});
        return;
    }

    // 1. Остановиться сразу; DriveSegment увидит Arrived и завершится.
    driver_.standstill();
    // 2. WAITING и отменяемые -> FAILED; неотменяемые (pick/drop) дорабатывают.
    action_executor_.cancel_all_order_actions();
    // 3. Маршрут убрать, update'ы этого order'а запретить; orderId/lastNodeId сохраняются.
    order_manager_.cancel();
    // 4. RUNNING, пока не завершатся все action'ы order'а.
    set_instant_status(a.action_id, v::ActionStatus::Running);
    pending_cancel_ids_.push_back(a.action_id);
    update_pending_cancel();
}

void ActionManager::update_pending_cancel() {
    if (pending_cancel_ids_.empty()) {
        return;
    }
    const bool all_done = std::all_of(State_.action_states.begin(), State_.action_states.end(),
                                      [](const auto& s) { return is_terminal(s.action_status); });
    if (!all_done) {
        return;
    }
    for (const auto& id : pending_cancel_ids_) {
        set_instant_status(id, v::ActionStatus::Finished);
    }
    pending_cancel_ids_.clear();
    publisher_.emit_state_changed();
}

// ---------------------------------------------------------------------------
// startPause / stopPause
// ---------------------------------------------------------------------------

void ActionManager::set_paused(const v::Action& a, bool paused) {
    State_.paused = paused;
    if (paused) {
        action_executor_.pause_all();    // движение остановит HoldPosition на следующем тике
    } else {
        action_executor_.resume_all();
    }
    set_instant_status(a.action_id, v::ActionStatus::Finished);
}

// ---------------------------------------------------------------------------
// initializePosition (6.2.3)
// ---------------------------------------------------------------------------

void ActionManager::initialize_position(const v::Action& a) {
    const auto x = number_param(a, "x");
    const auto y = number_param(a, "y");
    const auto theta = number_param(a, "theta");
    const auto map_id = string_param(a, "mapId");
    const auto last_node_id = string_param(a, "lastNodeId");

    auto fail = [&](const std::string& why) {
        set_instant_status(a.action_id, v::ActionStatus::Failed);
        add_warning(v::error_type::kInvalidInstantAction, why, {{"actionId", a.action_id}});
    };
    if (!x || !y || !theta || !map_id || !last_node_id) {
        return fail("initializePosition: x, y, theta, mapId, lastNodeId are required");
    }
    if (!v::is_idle(State_)) {
        return fail("initializePosition: robot has an active order");
    }
    if (State_.maps) {
        const bool enabled = std::any_of(State_.maps->begin(), State_.maps->end(), [&](const auto& m) {
            return m.map_id == *map_id && m.map_status == v::EnableStatus::Enabled;
        });
        if (!enabled) {
            return fail("initializePosition: map " + *map_id + " is not enabled");
        }
    }

    auto& pos = State_.mobile_robot_position.emplace(
        State_.mobile_robot_position.value_or(v::MobileRobotPosition{}));
    pos.x = *x;
    pos.y = *y;
    pos.theta = *theta;
    pos.map_id = *map_id;
    pos.localized = true;
    State_.last_node_id = *last_node_id;
    State_.last_node_sequence_id = 0;
    driver_.set_pose(*x, *y, *theta);

    set_instant_status(a.action_id, v::ActionStatus::Finished);
}

// ---------------------------------------------------------------------------
// Вспомогательные
// ---------------------------------------------------------------------------

void ActionManager::set_instant_status(const std::string& action_id, v::ActionStatus status) {
    for (auto& s : State_.instant_action_states) {
        if (s.action_id == action_id) {
            s.action_status = status;
            return;
        }
    }
}

void ActionManager::add_warning(const std::string& type, const std::string& description,
                                std::vector<v::ErrorReference> refs) {
    // Одна ошибка каждого типа: повтор заменяет прошлую.
    auto& errs = State_.errors;
    errs.erase(std::remove_if(errs.begin(), errs.end(),
                              [&](const v::Error& e) { return e.error_type == type; }),
               errs.end());
    v::Error e;
    e.error_type = type;
    e.error_level = v::ErrorLevel::Warning;
    e.error_description = description;
    if (!refs.empty()) {
        e.error_references = std::move(refs);
    }
    errs.push_back(std::move(e));
    spdlog::warn("[action_manager][{}] {}: {}", State_.header.serial_number, type, description);
}