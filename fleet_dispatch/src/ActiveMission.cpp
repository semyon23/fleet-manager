#include "../include/ActiveMission.hpp"
#include "../include/OrderBuilder.hpp"
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

    using Timestamp = google::protobuf::Timestamp;
    using MissionClock = ActiveMission::Clock;

    bool valid_timestamp(const Timestamp& value) {
        // Диапазон google.protobuf.Timestamp: от 0001 до 9999 года включительно.
        return value.seconds() >= -62135596800LL &&
               value.seconds() <= 253402300799LL &&
               value.nanos() >= 0 && value.nanos() < 1'000'000'000;
    }

    bool before(const Timestamp& lhs, const Timestamp& rhs) {
        return lhs.seconds() < rhs.seconds() ||
               (lhs.seconds() == rhs.seconds() && lhs.nanos() < rhs.nanos());
    }

    // Переводим возраст записи в шкалу steady_clock один раз при восстановлении.
    // После этого таймаут снова работает по монотонным часам.
    std::optional<MissionClock::time_point> restore_start(
        const Timestamp& start, const Timestamp& wall_now, MissionClock::time_point now) {
        using namespace std::chrono;

        if (!valid_timestamp(start) || !valid_timestamp(wall_now) || before(wall_now, start)) {
            return std::nullopt;
        }

        auto seconds_elapsed = wall_now.seconds() - start.seconds();
        auto nanos_elapsed = wall_now.nanos() - start.nanos();
        if (nanos_elapsed < 0) {
            --seconds_elapsed;
            nanos_elapsed += 1'000'000'000;
        }

        // Проверяем диапазон до перевода в более мелкие единицы steady_clock.
        const auto max_seconds = duration_cast<seconds>(MissionClock::duration::max());
        if (seconds_elapsed >= max_seconds.count()) return std::nullopt;

        const auto elapsed = duration_cast<MissionClock::duration>(
            seconds(seconds_elapsed) + nanoseconds(nanos_elapsed));
        if (now < MissionClock::time_point::min() + elapsed) return std::nullopt;

        return now - elapsed;
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

ActiveMission::ActiveMission(const fms::database::v1::MissionExecution& head,
                             int current, Clock::time_point mission_start,
                             Clock::time_point step_start)
    : recovering_move_(true),
      mission_(head.mission()),
      status_(head.status()),
      current_(current),
      mission_started_at_(mission_start),
      step_started_at_(step_start) {}

std::optional<ActiveMission> ActiveMission::restore_move(
    const fms::database::v1::RobotSpec& robot,
    const fms::database::v1::MissionExecution& head,
    const fms::database::v1::MissionExecution& grant,
    std::string_view request_id,
    const vda5050::State& state,
    Clock::time_point now,
    const google::protobuf::Timestamp& wall_now) {

    const auto match = move_recovery::reconcile(robot, head, grant, request_id, &state);
    if (match.decision != move_recovery::Decision::Attach) return std::nullopt;

    const auto& mission = head.mission();
    const auto& status = head.status();

    // У сохранённой последовательной миссии ровно один текущий шаг.
    // Проверяем также историю: предыдущие шаги завершены, следующие ещё ждут.
    int current = -1;
    for (int i = 0; i < mission.nodes_size(); ++i) {
        const auto& node = mission.nodes(i);
        const auto& step = status.steps(i);
        if (node.id() != step.node_id()) return std::nullopt;

        if (node.id() == status.current_node_id()) {
            if (current >= 0 || !node.has_move() ||
                step.state() != fms::v1::STEP_STATE_RUNNING) {
                return std::nullopt;
            }
            current = i;
        } else if (current < 0) {
            if (step.state() != fms::v1::STEP_STATE_COMPLETED &&
                step.state() != fms::v1::STEP_STATE_SKIPPED) {
                return std::nullopt;
            }
        } else if (step.state() != fms::v1::STEP_STATE_PENDING) {
            return std::nullopt;
        }
    }
    if (current < 0) return std::nullopt;

    const auto& node = mission.nodes(current);
    const auto& step = status.steps(current);

    auto mission_start = now;
    if (mission.has_timeout()) {
        if (!status.has_started_at()) return std::nullopt;
        const auto restored = restore_start(status.started_at(), wall_now, now);
        if (!restored) return std::nullopt;
        mission_start = *restored;
    }

    auto step_start = now;
    if (node.has_timeout()) {
        // При RETRY текущий код обновляет step_started_at_ только в памяти.
        // Сохранённый StepStatus.started_at относится к первой попытке.
        if (step.attempts() != 1 || !step.has_started_at()) return std::nullopt;
        if (status.has_started_at() && before(step.started_at(), status.started_at())) {
            return std::nullopt;
        }
        const auto restored = restore_start(step.started_at(), wall_now, now);
        if (!restored) return std::nullopt;
        step_start = *restored;
    }

    ActiveMission restored(head, current, mission_start, step_start);

    const auto& issued = grant.order();
    auto full = build_move_order(issued.order_id(), node.move());
    full.order_update_id = issued.order_update_id();

    restored.last_update_id_ = issued.order_update_id();
    restored.tracker_.emplace(full, now);
    restored.tracker_->on_state(state);

    // Именно этот state мог первым сообщить о прибытии на последний узел.
    // Применяем его здесь один раз, включая обычное обновление updated_at.
    restored.check_step(now);
    restored.changed_since(status);
    return restored;
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
    last_update_id_ = 0;   // все правки до этого момента — в самом order'е
}


std::uint32_t ActiveMission::released_nodes() const {
    const auto& move = current_step().move();
    return move.has_released_nodes() ? move.released_nodes() : static_cast<std::uint32_t>(move.nodes_size());
}

void ActiveMission::extend(std::uint32_t revision, std::uint32_t released_nodes) {
    // В самой миссии — чтобы повтор шага (RETRY) ушёл уже с новой base
    mission_.mutable_nodes(current_)->mutable_move()->set_released_nodes(released_nodes);
    auto* move = step_status().mutable_move();
    move->set_revision(revision);
    move->set_released_nodes(released_nodes);
    *status_.mutable_updated_at() = TimeUtil::GetCurrentTime();
}

void ActiveMission::update_sent(const vda5050::Order& update) {
    tracker_->update_sent(update.order_update_id);
    last_update_id_ = update.order_update_id;
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

    if (recovering_move_) {
        if (recovery_hold_ != RecoveryHold::None) return;

        update_progress();

        const bool mission_expired =
            mission_.has_timeout() &&
            now - mission_started_at_ > to_chrono(mission_.timeout());
        const bool step_expired =
            node.has_timeout() &&
            now - step_started_at_ > to_chrono(node.timeout());

        if (mission_expired || step_expired) {
            recovery_hold_ = RecoveryHold::Deadline;
            return;
        }

        if (!tracker_->done()) return;

        if (tracker_->phase() != OrderTracker::Phase::Finished) {
            recovery_hold_ = RecoveryHold::FailedOrder;
            return;
        }

        if (current_ + 1 < mission_.nodes_size()) {
            recovery_hold_ = RecoveryHold::NextStep;
            return;
        }

        step_succeeded(now);
        return;
    }

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
    // Правки маршрута: робот принял update с последней правкой (или order, если правки были до него)
    if (step.has_move() && tracker_->phase() != OrderTracker::Phase::Sent) {
        auto* move = step.mutable_move();
        if (tracker_->confirmed_update_id() >= last_update_id_) {
            move->set_confirmed_revision(move->revision());
        }
        move->set_update_rejected(tracker_->update_rejected());
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