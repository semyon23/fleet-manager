
#include "../include/OrderTracker.hpp"

namespace {

constexpr auto kAcceptTimeout = std::chrono::seconds(10);
}

OrderTracker::OrderTracker(const vda5050::Order& order, Clock::time_point sent_at)
    : order_id_(order.order_id),
      order_update_id_(order.order_update_id),
      sent_at_(sent_at),
      total_nodes_(static_cast<int>(order.nodes.size())) {
    // Запомнить все наши действия — на узлах и на рёбрах
    for (const auto& node : order.nodes) {
        for (const auto& action : node.actions) {
            actions_[action.action_id] = vda5050::ActionStatus::Waiting;
        }
    }
    for (const auto& edge : order.edges) {
        for (const auto& action : edge.actions) {
            actions_[action.action_id] = vda5050::ActionStatus::Waiting;
        }
    }
}

void OrderTracker::on_state(const vda5050::State& state) {
    if (done()) {
        return;
    }
    bool ours = state.order_id == order_id_;

    // 1. Ждём, пока робот примет order
    if (phase_ == Phase::Sent) {
        if (ours && state.order_update_id == order_update_id_) {
            phase_ = Phase::Accepted;
        } else {
            // При отказе робот оставляет старый order и пишет ошибку со ссылкой на наш orderId
            if (auto error = order_error(state); !error.empty()) {
                finish(Phase::Rejected, error);
            }
            return;
        }
    }

    // 2. Принят. Если в state уже другой orderId — наш заменили в обход dispatch
    if (!ours) {
        finish(Phase::Failed, "robot switched to order " + state.order_id);
        return;
    }

    // 3. Прогресс: узел с sequenceId 2*i — это узел i маршрута
    if (!state.last_node_id.empty()) {
        passed_nodes_ = static_cast<int>(state.last_node_sequence_id / 2) + 1;
    }

    // 4. Статусы наших действий. Провал действия запоминаем, но order не заканчиваем:
    //    робот может ещё ехать — ждём, пока он закончит, иначе следующий order наткнётся на занятого робота
    for (const auto& action_state : state.action_states) {
        auto it = actions_.find(action_state.action_id);
        if (it != actions_.end()) {
            it->second = action_state.action_status;
        }
    }

    // 5. Base кончилась: впереди только horizon (released: false), робот стоит
    bool only_horizon_left = std::all_of(state.node_states.begin(), state.node_states.end(),
                                         [](const auto& node) { return !node.released; });
    waiting_for_release_ = !state.node_states.empty() && only_horizon_left && !state.driving;

    // 6. Order закончился: маршрут пройден, робот стоит, каждое наше действие FINISHED или FAILED
    bool route_done = state.node_states.empty() && state.edge_states.empty() && !state.driving;
    bool actions_final = std::all_of(actions_.begin(), actions_.end(), [](const auto& action) {
        return action.second == vda5050::ActionStatus::Finished || action.second == vda5050::ActionStatus::Failed;
    });
    if (!route_done || !actions_final) {
        return;
    }
    auto failed = std::find_if(actions_.begin(), actions_.end(), [](const auto& action) {
        return action.second == vda5050::ActionStatus::Failed;
    });
    if (failed != actions_.end()) {
        finish(Phase::Failed, "action " + failed->first + " failed");
    } else {
        finish(Phase::Finished);
    }
}

void OrderTracker::on_tick(Clock::time_point now) {
    if (phase_ == Phase::Sent && now - sent_at_ > kAcceptTimeout) {
        finish(Phase::Rejected, "robot did not accept order in 10 s");
    }
}

void OrderTracker::finish(Phase phase, std::string failure) {
    phase_ = phase;
    failure_ = std::move(failure);
}

std::string OrderTracker::order_error(const vda5050::State& state) const {
    for (const auto& error : state.errors) {
        if (!error.error_references) {
            continue;
        }
        for (const auto& reference : *error.error_references) {
            if (reference.reference_key == "orderId" && reference.reference_value == order_id_) {
                return error.error_type + ": " + error.error_description.value_or("");
            }
        }
    }
    return {};
}

const char* to_string(OrderTracker::Phase phase) {
    switch (phase) {
        case OrderTracker::Phase::Sent:     return "SENT";
        case OrderTracker::Phase::Accepted: return "ACCEPTED";
        case OrderTracker::Phase::Rejected: return "REJECTED";
        case OrderTracker::Phase::Finished: return "FINISHED";
        case OrderTracker::Phase::Failed:   return "FAILED";
    }
    return "?";
}