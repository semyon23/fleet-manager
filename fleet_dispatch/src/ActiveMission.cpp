
#include "../include/ActiveMission.hpp"

#include <algorithm>
#include <google/protobuf/util/message_differencer.h>
#include <google/protobuf/util/time_util.h>

using google::protobuf::util::TimeUtil;

namespace {

fms::v1::ActionStatus to_proto(vda5050::ActionStatus status) {
    switch (status) {
        case vda5050::ActionStatus::Waiting:      return fms::v1::ACTION_STATUS_WAITING;
        case vda5050::ActionStatus::Initializing: return fms::v1::ACTION_STATUS_INITIALIZING;
        case vda5050::ActionStatus::Running:      return fms::v1::ACTION_STATUS_RUNNING;
        case vda5050::ActionStatus::Paused:       return fms::v1::ACTION_STATUS_PAUSED;
        case vda5050::ActionStatus::Retriable:    return fms::v1::ACTION_STATUS_RETRIABLE;
        case vda5050::ActionStatus::Finished:     return fms::v1::ACTION_STATUS_FINISHED;
        case vda5050::ActionStatus::Failed:       return fms::v1::ACTION_STATUS_FAILED;
    }
    return fms::v1::ACTION_STATUS_UNSPECIFIED;
}

void add_action(fms::v1::StepStatus* step, const vda5050::Action& action, const std::string& where) {
    auto* progress = step->add_actions();
    progress->set_action_id(action.action_id);
    progress->set_action_type(action.action_type);
    progress->set_node_id(where);   // id узла или ребра
    progress->set_status(fms::v1::ACTION_STATUS_WAITING);
}

}  // namespace

ActiveMission::ActiveMission(fms::v1::Mission mission, const vda5050::Order& order,
                             OrderTracker::Clock::time_point sent_at)
    : mission_(std::move(mission)), tracker_(order, sent_at) {
    auto now = TimeUtil::GetCurrentTime();
    const auto& node = mission_.nodes(0);

    status_.set_mission_id(mission_.mission_id());
    status_.set_robot_id(mission_.robot_id());
    status_.set_state(fms::v1::MISSION_STATE_RUNNING);
    status_.set_current_node_id(node.id());
    *status_.mutable_started_at() = now;
    *status_.mutable_updated_at() = now;

    auto* step = status_.add_steps();
    step->set_node_id(node.id());
    step->set_state(fms::v1::STEP_STATE_RUNNING);
    step->set_attempts(1);
    *step->mutable_started_at() = now;

    if (node.has_move()) {
        auto* move = step->mutable_move();
        move->set_last_node_id(order.nodes.front().node_id);
        move->set_last_node_index(0);
        move->set_total_nodes(static_cast<std::uint32_t>(order.nodes.size()));
        move->set_released_nodes(static_cast<std::uint32_t>(
            std::count_if(order.nodes.begin(), order.nodes.end(), [](const auto& n) { return n.released; })));
    }

    for (const auto& order_node : order.nodes) {
        for (const auto& action : order_node.actions) {
            add_action(step, action, order_node.node_id);
        }
    }
    for (const auto& edge : order.edges) {
        for (const auto& action : edge.actions) {
            add_action(step, action, edge.edge_id);
        }
    }
}

bool ActiveMission::on_state(const vda5050::State& state) {
    auto before = status_;
    tracker_.on_state(state);
    update_status();
    return !google::protobuf::util::MessageDifferencer::Equals(before, status_);
}

bool ActiveMission::on_tick(OrderTracker::Clock::time_point now) {
    auto before = status_;
    tracker_.on_tick(now);
    update_status();
    return !google::protobuf::util::MessageDifferencer::Equals(before, status_);
}

void ActiveMission::update_status() {
    auto* step = status_.mutable_steps(0);

    // Прогресс по маршруту
    if (step->has_move() && tracker_.passed_nodes() > 0) {
        int index = tracker_.passed_nodes() - 1;
        auto* move = step->mutable_move();
        move->set_last_node_index(static_cast<std::uint32_t>(index));
        move->set_last_node_id(mission_.nodes(0).move().nodes(index).node_id());
        move->set_waiting_for_release(tracker_.waiting_for_release());
    }

    // Статусы действий
    for (auto& action : *step->mutable_actions()) {
        auto it = tracker_.actions().find(action.action_id());
        if (it != tracker_.actions().end()) {
            action.set_status(to_proto(it->second));
        }
    }

    if (!tracker_.done()) {
        return;
    }

    // Order закончился — с ним и шаг, и миссия (шаг один)
    auto now = TimeUtil::GetCurrentTime();
    *step->mutable_finished_at() = now;
    *status_.mutable_finished_at() = now;
    status_.clear_current_node_id();

    if (tracker_.phase() == OrderTracker::Phase::Finished) {
        step->set_state(fms::v1::STEP_STATE_COMPLETED);
        status_.set_state(fms::v1::MISSION_STATE_COMPLETED);
        return;
    }

    step->set_state(fms::v1::STEP_STATE_FAILED);
    status_.set_state(fms::v1::MISSION_STATE_FAILED);
    auto* error = status_.mutable_error();
    error->set_message(tracker_.failure());

    bool action_failed = std::any_of(tracker_.actions().begin(), tracker_.actions().end(), [](const auto& a) {
        return a.second == vda5050::ActionStatus::Failed;
    });
    if (tracker_.phase() == OrderTracker::Phase::Rejected) {
        error->set_code(fms::v1::MISSION_ERROR_CODE_ORDER_REJECTED);
    } else if (action_failed) {
        error->set_code(fms::v1::MISSION_ERROR_CODE_ACTION_FAILED);
    } else {
        error->set_code(fms::v1::MISSION_ERROR_CODE_ROBOT_ERROR);   // робот переключился на чужой order
    }
}