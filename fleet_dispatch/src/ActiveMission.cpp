#include "../include/ActiveMission.hpp"

#include <algorithm>
#include <google/protobuf/util/message_differencer.h>
#include <google/protobuf/util/time_util.h>

using google::protobuf::util::TimeUtil;

namespace {

constexpr auto kCancelTimeout = std::chrono::seconds(10);

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

void add_action(fms::v1::StepStatus& step, const vda5050::Action& action, const std::string& where) {
    auto* progress = step.add_actions();
    progress->set_action_id(action.action_id);
    progress->set_action_type(action.action_type);
    progress->set_node_id(where);   // id узла или ребра
    progress->set_status(fms::v1::ACTION_STATUS_WAITING);
}

std::chrono::milliseconds to_chrono(const google::protobuf::Duration& duration) {
    return std::chrono::milliseconds(TimeUtil::DurationToMilliseconds(duration));
}

}  // namespace

ActiveMission::ActiveMission(fms::v1::Mission mission, Clock::time_point now)
    : mission_(std::move(mission)), mission_started_at_(now) {
    auto timestamp = TimeUtil::GetCurrentTime();
    status_.set_mission_id(mission_.mission_id());
    status_.set_robot_id(mission_.robot_id());
    status_.set_state(fms::v1::MISSION_STATE_RUNNING);
    *status_.mutable_started_at() = timestamp;
    *status_.mutable_updated_at() = timestamp;

    for (const auto& node : mission_.nodes()) {
        auto* step = status_.add_steps();
        step->set_node_id(node.id());
        step->set_state(fms::v1::STEP_STATE_PENDING);
    }
    start_step(now);
}

// ------------------------------------------------------------------ order шага

bool ActiveMission::order_needed() const {
    if (done() || canceling() || tracker_) {
        return false;
    }
    return current_step().has_move() || current_step().has_action();
}

// mission-шаг; при повторе — с номером попытки: тот же orderId робот второй раз не примет
std::string ActiveMission::current_order_id() const {
    std::string order_id = mission_.mission_id() + "-" + current_step().id();
    std::uint32_t attempt = status_.steps(current_).attempts();
    if (attempt > 1) {
        order_id += "-a" + std::to_string(attempt);
    }
    return order_id;
}

void ActiveMission::order_sent(const vda5050::Order& order, Clock::time_point now) {
    tracker_.emplace(order, now);
    auto& step = step_status();

    // Действия — из построенного order'а: там actionId уже заполнены
    step.clear_actions();
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

    if (current_step().has_move()) {
        auto* move = step.mutable_move();
        move->set_last_node_id(order.nodes.front().node_id);
        move->set_last_node_index(0);
        move->set_total_nodes(static_cast<std::uint32_t>(order.nodes.size()));
        move->set_released_nodes(static_cast<std::uint32_t>(
            std::count_if(order.nodes.begin(), order.nodes.end(), [](const auto& n) { return n.released; })));
    }
}

// ------------------------------------------------------------------ события

bool ActiveMission::on_state(const vda5050::State& state) {
    if (done()) {
        return false;
    }
    auto before = status_;
    if (canceling()) {
        check_cancel(state);
    } else if (tracker_) {
        tracker_->on_state(state);
        check_step(Clock::now());
    }
    return changed_since(before);
}

bool ActiveMission::on_tick(Clock::time_point now) {
    if (done()) {
        return false;
    }
    auto before = status_;
    if (canceling()) {
        if (now - cancel_sent_at_ > kCancelTimeout) {
            fail(fms::v1::MISSION_ERROR_CODE_TIMEOUT, "robot did not confirm cancelOrder in 10 s");
        }
    } else {
        if (tracker_) {
            tracker_->on_tick(now);
        }
        check_step(now);   // ожидание и таймауты идут и без state'ов
    }
    return changed_since(before);
}

// ------------------------------------------------------------------ шаги

void ActiveMission::start_step(Clock::time_point now) {
    auto& step = step_status();
    step.set_state(fms::v1::STEP_STATE_RUNNING);
    step.set_attempts(1);
    *step.mutable_started_at() = TimeUtil::GetCurrentTime();
    status_.set_current_node_id(current_step().id());
    step_started_at_ = now;
    tracker_.reset();
}

void ActiveMission::check_step(Clock::time_point now) {
    const auto& node = current_step();

    // Таймауты: вся миссия и текущий шаг (у шага — на каждую попытку)
    if (mission_.has_timeout() && now - mission_started_at_ > to_chrono(mission_.timeout())) {
        fail(fms::v1::MISSION_ERROR_CODE_TIMEOUT, "mission timeout");
        return;
    }
    if (node.has_timeout() && now - step_started_at_ > to_chrono(node.timeout())) {
        fail(fms::v1::MISSION_ERROR_CODE_TIMEOUT, "step " + node.id() + " timeout");
        return;
    }

    // Ожидание: шаг кончается сам по времени
    if (node.has_wait()) {
        if (now - step_started_at_ >= to_chrono(node.wait().duration())) {
            step_succeeded(now);
        }
        return;
    }

    // Шаг с order'ом
    if (!tracker_) {
        return;   // order ещё не отправлен
    }
    update_progress();
    if (!tracker_->done()) {
        return;
    }
    switch (tracker_->phase()) {
        case OrderTracker::Phase::Finished:
            step_succeeded(now);
            break;
        case OrderTracker::Phase::Rejected:
            step_failed(fms::v1::MISSION_ERROR_CODE_ORDER_REJECTED, tracker_->failure(), now);
            break;
        default: {
            bool action_failed = std::any_of(tracker_->actions().begin(), tracker_->actions().end(),
                                             [](const auto& a) { return a.second == vda5050::ActionStatus::Failed; });
            if (action_failed) {
                step_failed(fms::v1::MISSION_ERROR_CODE_ACTION_FAILED, tracker_->failure(), now);
            } else {
                // робот выполняет чужой order — повторять бессмысленно
                fail(fms::v1::MISSION_ERROR_CODE_ROBOT_ERROR, tracker_->failure());
            }
        }
    }
}

void ActiveMission::step_succeeded(Clock::time_point now) {
    auto& step = step_status();
    step.set_state(fms::v1::STEP_STATE_COMPLETED);
    *step.mutable_finished_at() = TimeUtil::GetCurrentTime();

    ++current_;
    if (current_ == mission_.nodes_size()) {
        finish(fms::v1::MISSION_STATE_COMPLETED);
        return;
    }
    start_step(now);
}

void ActiveMission::step_failed(fms::v1::MissionErrorCode code, const std::string& message, Clock::time_point now) {
    const auto& node = current_step();
    auto& step = step_status();

    // RETRY: попыток было attempts, разрешено 1 + max_retries
    if (node.on_failure() == fms::v1::FAILURE_ACTION_RETRY && step.attempts() <= node.max_retries()) {
        step.set_attempts(step.attempts() + 1);
        step_started_at_ = now;
        tracker_.reset();   // сессия отправит order заново (order_needed)
        return;
    }
    if (node.on_failure() == fms::v1::FAILURE_ACTION_SKIP) {
        step.set_state(fms::v1::STEP_STATE_SKIPPED);
        *step.mutable_finished_at() = TimeUtil::GetCurrentTime();
        ++current_;
        if (current_ == mission_.nodes_size()) {
            finish(fms::v1::MISSION_STATE_COMPLETED);
            return;
        }
        start_step(now);
        return;
    }
    fail(code, message);
}

void ActiveMission::update_progress() {
    auto& step = step_status();
    if (step.has_move() && tracker_->passed_nodes() > 0) {
        int index = tracker_->passed_nodes() - 1;
        auto* move = step.mutable_move();
        move->set_last_node_index(static_cast<std::uint32_t>(index));
        move->set_last_node_id(current_step().move().nodes(index).node_id());
        move->set_waiting_for_release(tracker_->waiting_for_release());
    }
    for (auto& action : *step.mutable_actions()) {
        auto it = tracker_->actions().find(action.action_id());
        if (it != tracker_->actions().end()) {
            action.set_status(to_proto(it->second));
        }
    }
}

// ------------------------------------------------------------------ отмена и конец

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
        // FINISHED — order отменён; FAILED — отменять было нечего. Робот стоит в обоих случаях
        if (action_state.action_status == vda5050::ActionStatus::Finished ||
            action_state.action_status == vda5050::ActionStatus::Failed) {
            auto& step = step_status();
            step.set_state(fms::v1::STEP_STATE_CANCELED);
            *step.mutable_finished_at() = TimeUtil::GetCurrentTime();
            finish(fms::v1::MISSION_STATE_CANCELED);
        }
        return;
    }
}

void ActiveMission::abort(fms::v1::MissionErrorCode code, const std::string& message) {
    if (done()) {
        return;
    }
    fail(code, message);
    *status_.mutable_updated_at() = TimeUtil::GetCurrentTime();
}

bool ActiveMission::done() const {
    auto state = status_.state();
    return state == fms::v1::MISSION_STATE_COMPLETED || state == fms::v1::MISSION_STATE_FAILED ||
           state == fms::v1::MISSION_STATE_CANCELED;
}

// Текущий шаг FAILED (если он ещё шёл), миссия FAILED с причиной
void ActiveMission::fail(fms::v1::MissionErrorCode code, const std::string& message) {
    auto& step = step_status();
    if (step.state() == fms::v1::STEP_STATE_RUNNING) {
        step.set_state(fms::v1::STEP_STATE_FAILED);
        *step.mutable_finished_at() = TimeUtil::GetCurrentTime();
    }
    finish(fms::v1::MISSION_STATE_FAILED);
    auto* error = status_.mutable_error();
    error->set_code(code);
    error->set_message(message);
}

void ActiveMission::finish(fms::v1::MissionState state) {
    status_.set_state(state);
    *status_.mutable_finished_at() = TimeUtil::GetCurrentTime();
    status_.clear_current_node_id();
}

bool ActiveMission::changed_since(const fms::v1::MissionStatus& before) {
    bool changed = !google::protobuf::util::MessageDifferencer::Equals(before, status_);
    if (changed) {
        *status_.mutable_updated_at() = TimeUtil::GetCurrentTime();
    }
    return changed;
}