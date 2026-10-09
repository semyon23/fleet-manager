// RoboticVehicleSimulator: instant action'ы (VDA 5050 v3.0.0, 6.1.3, 6.2.1, 6.2.3).
//
// Дерево поведения instant action'ы не получает: робот меняет State_ и флаги,
// а дерево на следующем тике видит это через IsPaused / HasActiveOrder.
//
// handle_instant_actions() вызывает транспорт через asio::post(executor(), ...).
// update_pending_cancel() — вызывать в simulation_tick() после actions_.update(dt).

#include "RoboticVehicleSimulator.hpp"

#include <algorithm>
#include <iostream>

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

// ---------------------------------------------------------------------------
// Вход
// ---------------------------------------------------------------------------

void RoboticVehicleSimulator::handle_instant_actions(const std::string& json) {
    v::InstantActions msg;
    try {
        msg = v::json::parse(json).get<v::InstantActions>();
    } catch (const std::exception& e) {
        add_warning(v::error_type::kValidationFailure,
                    std::string("Malformed instantActions: ") + e.what(), {});
        publisher_.emit();
        return;
    }

    for (const auto& action : msg.actions) {
        // Повтор того же actionId (fms не дождался state) — не выполняем второй раз.
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
    publisher_.emit();
}

void RoboticVehicleSimulator::execute_instant_action(const v::Action& a) {
    const auto& type = a.action_type;

    if (type == v::action_type::kCancelOrder)            cancel_order(a);
    else if (type == v::action_type::kStartPause)        set_paused(a, true);
    else if (type == v::action_type::kStopPause)         set_paused(a, false);
    else if (type == v::action_type::kStateRequest) {
        set_instant_status(a.action_id, v::ActionStatus::Finished);
        publisher_.publish_state_now();                  // уже со статусом FINISHED
    }
    else if (type == v::action_type::kFactsheetRequest) {
        publisher_.publish_factsheet();
        set_instant_status(a.action_id, v::ActionStatus::Finished);
    }
    else if (type == v::action_type::kInitializePosition) initialize_position(a);
    else if (actions_.supported_types().count(type)) {
        // Свои instant action'ы — через исполнитель (blockingType у instant всегда NONE).
        actions_.enqueue({a}, /*instant=*/true);
    } else {
        set_instant_status(a.action_id, v::ActionStatus::Failed);
        add_warning(v::error_type::kInvalidInstantAction, "Unsupported instant action " + type,
                    {{"actionId", a.action_id}, {"actionType", type}});
    }
}

// ---------------------------------------------------------------------------
// cancelOrder (6.1.3)
// ---------------------------------------------------------------------------

void RoboticVehicleSimulator::cancel_order(const v::Action& a) {
    const auto order_id = string_param(a, "orderId");
    const bool nothing_to_cancel =
        v::is_idle(State_) || (order_id && *order_id != State_.order_id);
    if (nothing_to_cancel) {
        // 6.1.3.2: нечего отменять — FAILED + NO_ORDER_TO_CANCEL.
        set_instant_status(a.action_id, v::ActionStatus::Failed);
        add_warning(v::error_type::kNoOrderToCancel, "No active order to cancel",
                    {{"actionId", a.action_id}});
        return;
    }

    // 1. Остановиться сразу: DriveSegment сам не заметит, что маршрут отменён.
    stop_driving();
    // 2. WAITING и отменяемые — FAILED; неотменяемые (pick/drop) дорабатывают.
    actions_.cancel_all_order_actions();
    // 3. Маршрута больше нет; orderId, orderUpdateId, lastNodeId сохраняются (6.6.7).
    State_.node_states.clear();
    State_.edge_states.clear();
    State_.new_base_request = false;
    order_canceled_ = true;
    // 4. RUNNING, пока не завершатся все action'ы order'а.
    set_instant_status(a.action_id, v::ActionStatus::Running);
    pending_cancel_id_ = a.action_id;
    update_pending_cancel();
}

void RoboticVehicleSimulator::update_pending_cancel() {
    if (!pending_cancel_id_) {
        return;
    }
    const bool all_done = std::all_of(State_.action_states.begin(), State_.action_states.end(),
                                      [](const auto& s) { return is_terminal(s.action_status); });
    if (all_done) {
        set_instant_status(*pending_cancel_id_, v::ActionStatus::Finished);
        pending_cancel_id_.reset();
        publisher_.emit();
    }
}

// ---------------------------------------------------------------------------
// startPause / stopPause
// ---------------------------------------------------------------------------

void RoboticVehicleSimulator::set_paused(const v::Action& a, bool paused) {
    State_.paused = paused;
    if (paused) {
        actions_.pause_all();     // pauseAllowed-action'ы → PAUSED; движение остановит HoldPosition
    } else {
        actions_.resume_all();    // дерево продолжит с текущей позиции
    }
    set_instant_status(a.action_id, v::ActionStatus::Finished);
}

// ---------------------------------------------------------------------------
// initializePosition (6.2.3, таблица 4/5)
// ---------------------------------------------------------------------------

void RoboticVehicleSimulator::initialize_position(const v::Action& a) {
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

void RoboticVehicleSimulator::set_instant_status(const std::string& action_id, v::ActionStatus status) {
    for (auto& s : State_.instant_action_states) {
        if (s.action_id == action_id) {
            s.action_status = status;
            return;
        }
    }
}

void RoboticVehicleSimulator::add_warning(const std::string& type, const std::string& description,
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
    std::cerr << "[" << config_.name() << "] " << type << ": " << description << '\n';
}
