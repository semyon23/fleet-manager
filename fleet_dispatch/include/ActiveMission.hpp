
#ifndef FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
#define FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
#pragma once

#include <string>

#include "OrderTracker.hpp"
#include "fms_types.pb.h"

class ActiveMission {
public:
    using Clock = OrderTracker::Clock;

    ActiveMission(fms::v1::Mission mission, const vda5050::Order& order, OrderTracker::Clock::time_point sent_at);

    bool on_state(const vda5050::State& state);
    bool on_tick(OrderTracker::Clock::time_point now);

    void cancel(const std::string& cancel_action_id, Clock::time_point now);
    bool done() const;
    bool canceling() const { return !cancel_action_id_.empty() && !done(); }

    const std::string& mission_id() const { return mission_.mission_id(); }
    const fms::v1::MissionStatus& status() const { return status_; }

private:
    void update_status();
    void check_cancel(const vda5050::State& state);
    void finish(fms::v1::MissionState mission_state, fms::v1::StepState step_state);
    void fail(fms::v1::MissionErrorCode code, const std::string& message);

    fms::v1::Mission mission_;
    OrderTracker tracker_;
    fms::v1::MissionStatus status_;

    std::string cancel_action_id_;
    Clock::time_point cancel_sent_at_;
};
#endif //FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
