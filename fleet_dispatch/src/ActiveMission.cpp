
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

}

namespace {
    constexpr auto kCancelTimeout = std::chrono::seconds(10);   // 5d
}


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
    if (done()) {
        return false;
    }
    auto before = status_;
    if (canceling()) {
        check_cancel(state);
    } else {
        tracker_.on_state(state);
        update_status();
    }
    bool changed = !google::protobuf::util::MessageDifferencer::Equals(before, status_);
    if (changed) {
        *status_.mutable_updated_at() = TimeUtil::GetCurrentTime();   // 6
    }
    return changed;
}

bool ActiveMission::on_tick(OrderTracker::Clock::time_point now) {
    if (done()) {
        return false;
    }
    auto before = status_;
    if (canceling()) {
        if (now - cancel_sent_at_ > kCancelTimeout) {
            fail(fms::v1::MISSION_ERROR_CODE_TIMEOUT, "robot did not confirm cancelOrder in 10 s");
        }
    } else {
        tracker_.on_tick(now);
        update_status();
    }
    bool changed = !google::protobuf::util::MessageDifferencer::Equals(before, status_);
    if (changed) {
        *status_.mutable_updated_at() = TimeUtil::GetCurrentTime();   // 6
    }
    return changed;
}

void ActiveMission::cancel(const std::string& cancel_action_id, Clock::time_point now) {
    cancel_action_id_ = cancel_action_id;
    cancel_sent_at_ = now;
    status_.set_state(fms::v1::MISSION_STATE_CANCELING);
    *status_.mutable_updated_at() = TimeUtil::GetCurrentTime();
}

void ActiveMission::check_cancel(const vda5050::State& state) {
    for (const auto& action_state : state.instant_action_states) {
        if (action_state.action_id != cancel_action_id_) {
            continue;
        }

        if (action_state.action_status == vda5050::ActionStatus::Finished ||
            action_state.action_status == vda5050::ActionStatus::Failed) {
            finish(fms::v1::MISSION_STATE_CANCELED, fms::v1::STEP_STATE_CANCELED);
            }
        return;
    }
}



void ActiveMission::update_status() {
    auto* step = status_.mutable_steps(0);

    if (step->has_move() && tracker_.passed_nodes() > 0) {
        int index = tracker_.passed_nodes() - 1;
        auto* move = step->mutable_move();
        move->set_last_node_index(static_cast<std::uint32_t>(index));
        move->set_last_node_id(mission_.nodes(0).move().nodes(index).node_id());
        move->set_waiting_for_release(tracker_.waiting_for_release());
    }

    for (auto& action : *step->mutable_actions()) {
        auto it = tracker_.actions().find(action.action_id());
        if (it != tracker_.actions().end()) {
            action.set_status(to_proto(it->second));
        }
    }

    if (!tracker_.done()) {
        return;
    }
    if (tracker_.phase() == OrderTracker::Phase::Finished) {
        finish(fms::v1::MISSION_STATE_COMPLETED, fms::v1::STEP_STATE_COMPLETED);
        return;
    }

    bool action_failed = std::any_of(tracker_.actions().begin(), tracker_.actions().end(), [](const auto& a) {
        return a.second == vda5050::ActionStatus::Failed;
    });
    if (tracker_.phase() == OrderTracker::Phase::Rejected) {
        fail(fms::v1::MISSION_ERROR_CODE_ORDER_REJECTED, tracker_.failure());
    } else if (action_failed) {
        fail(fms::v1::MISSION_ERROR_CODE_ACTION_FAILED, tracker_.failure());
    } else {
        fail(fms::v1::MISSION_ERROR_CODE_ROBOT_ERROR, tracker_.failure());   // робот переключился на чужой order
    }
}

bool ActiveMission::done() const {
    auto state = status_.state();
    return state == fms::v1::MISSION_STATE_COMPLETED || state == fms::v1::MISSION_STATE_FAILED ||
           state == fms::v1::MISSION_STATE_CANCELED;
}

void ActiveMission::fail(fms::v1::MissionErrorCode code, const std::string &message) {
    finish(fms::v1::MISSION_STATE_FAILED, fms::v1::STEP_STATE_FAILED);
    auto* error = status_.mutable_error();
    error->set_code(code);
    error->set_message(message);
}

void ActiveMission::finish(fms::v1::MissionState mission_state, fms::v1::StepState step_state) {
    auto now = TimeUtil::GetCurrentTime();
    auto* step = status_.mutable_steps(0);
    step->set_state(step_state);
    *step->mutable_finished_at() = now;
    status_.set_state(mission_state);
    *status_.mutable_finished_at() = now;
    status_.clear_current_node_id();
}

void ActiveMission::abort(fms::v1::MissionErrorCode code, const std::string &message) {
    if (done()) {
        return;
    }
    fail(code, message);
    *status_.mutable_updated_at() = TimeUtil::GetCurrentTime();
}
