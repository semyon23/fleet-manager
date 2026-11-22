#ifndef FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
#define FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
#pragma once

#include <algorithm>
#include <optional>
#include <string>

#include "OrderTracker.hpp"
#include "MoveRecovery.hpp"
#include "fms_types.pb.h"

// Миссия, которую сейчас выполняет робот. Принадлежит RobotSession.
// Шаги выполняются по порядку. ActiveMission решает, какой шаг сейчас и чем он кончился;
// order для шага строит и отправляет сессия (order_needed → order_sent).
class ActiveMission {
public:
    using Clock = OrderTracker::Clock;

    ActiveMission(fms::v1::Mission mission, Clock::time_point now);

    enum class RecoveryHold {
        None,
        Deadline,
        NextStep,
        FailedOrder
    };

    // Вызывающий передаёт актуальный state с токеном текущей попытки опроса.
    // Пустой результат — присоединение по этим данным запрещено.
    static std::optional<ActiveMission> restore_move(
        const fms::database::v1::RobotSpec& robot,
        const fms::database::v1::MissionExecution& head,
        const fms::database::v1::MissionExecution& grant,
        std::string_view request_id,
        const vda5050::State& state,
        Clock::time_point now,
        const google::protobuf::Timestamp& wall_now);

    RecoveryHold recovery_hold() const { return recovery_hold_; }

    // Текущему шагу (Move / Action) нужен order, а он ещё не отправлен — первый раз или повтор
    bool order_needed() const;
    const fms::v1::MissionNode& current_step() const { return mission_.nodes(current_); }
    std::string current_order_id() const;
    void order_sent(const vda5050::Order& order, Clock::time_point now);

    // ExtendMission (T1). Проверки — в DispatchService, здесь только применить.
    // Сколько узлов текущего шага MoveStep выдано (или уйдёт в base первым order'ом)
    std::uint32_t released_nodes() const;
    // Правка revision: base текущего шага — до released_nodes узлов
    void extend(std::uint32_t revision, std::uint32_t released_nodes);
    // Order update с правкой отправлен роботу
    void update_sent(const vda5050::Order& update);
    // Order текущего шага у робота; nullptr — ещё не отправлен (или шаг без order'а)
    const OrderTracker* tracker() const { return tracker_ ? &*tracker_ : nullptr; }
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
    const fms::v1::Mission& mission() const { return mission_; }   // с правками ExtendMission
private:

    // Отдельный вход для сохранённой миссии. Обычный конструктор запускает шаг 0.
    ActiveMission(const fms::database::v1::MissionExecution& head, int current,
                  Clock::time_point mission_start, Clock::time_point step_start);

    bool recovering_move_ = false;
    RecoveryHold recovery_hold_ = RecoveryHold::None;
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
    std::uint32_t last_update_id_ = 0;        // orderUpdateId, в котором ушла последняя правка (0 — в самом order'е)
    Clock::time_point mission_started_at_;
    Clock::time_point step_started_at_;

    std::string cancel_action_id_;
    Clock::time_point cancel_sent_at_;
};

#endif //FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP