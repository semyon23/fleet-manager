#ifndef FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
#define FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
#pragma once

#include <algorithm>
#include <optional>
#include <string>

#include "OrderTracker.hpp"
#include "fms_types.pb.h"

// Миссия, которую сейчас выполняет робот. Принадлежит RobotSession.
// Шаги выполняются по порядку. ActiveMission решает, какой шаг сейчас и чем он кончился;
// order для шага строит и отправляет сессия (order_needed → order_sent).
class ActiveMission {
public:
    using Clock = OrderTracker::Clock;

    ActiveMission(fms::v1::Mission mission, Clock::time_point now);

    // Текущему шагу (Move / Action) нужен order, а он ещё не отправлен — первый раз или повтор
    bool order_needed() const;
    const fms::v1::MissionNode& current_step() const { return mission_.nodes(current_); }
    std::string current_order_id() const;
    void order_sent(const vda5050::Order& order, Clock::time_point now);

    // true — статус миссии изменился (залогировать, отдать подписчикам, отправить order следующего шага)
    bool on_state(const vda5050::State& state);
    bool on_tick(Clock::time_point now);

    void cancel(const std::string& cancel_action_id, Clock::time_point now);
    void abort(fms::v1::MissionErrorCode code, const std::string& message);

    bool done() const;
    bool canceling() const { return !cancel_action_id_.empty() && !done(); }
    int current_index() const { return std::min(current_, mission_.nodes_size() - 1); }
    const std::string& mission_id() const { return mission_.mission_id(); }
    const fms::v1::MissionStatus& status() const { return status_; }

private:
    void start_step(Clock::time_point now);
    void check_step(Clock::time_point now);   // шаг закончился? таймауты?
    void step_succeeded(Clock::time_point now);
    void step_failed(fms::v1::MissionErrorCode code, const std::string& message, Clock::time_point now);
    void update_progress();                   // прогресс шага по tracker'у
    void check_cancel(const vda5050::State& state);
    void finish(fms::v1::MissionState state);
    void fail(fms::v1::MissionErrorCode code, const std::string& message);
    bool changed_since(const fms::v1::MissionStatus& before);
    fms::v1::StepStatus& step_status() { return *status_.mutable_steps(current_); }

    fms::v1::Mission mission_;
    fms::v1::MissionStatus status_;
    int current_ = 0;                         // индекс текущего шага
    std::optional<OrderTracker> tracker_;     // order текущего шага; пусто — ещё не отправлен или шаг без order'а
    Clock::time_point mission_started_at_;
    Clock::time_point step_started_at_;

    std::string cancel_action_id_;
    Clock::time_point cancel_sent_at_;
};

#endif //FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP