
#ifndef FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
#define FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
#pragma once

#include <string>

#include "OrderTracker.hpp"
#include "fms_types.pb.h"

class ActiveMission {
public:
    ActiveMission(fms::v1::Mission mission, const vda5050::Order& order, OrderTracker::Clock::time_point sent_at);

    bool on_state(const vda5050::State& state);
    bool on_tick(OrderTracker::Clock::time_point now);

    bool done() const { return tracker_.done(); }
    const std::string& mission_id() const { return mission_.mission_id(); }
    const fms::v1::MissionStatus& status() const { return status_; }

private:
    void update_status();

    fms::v1::Mission mission_;
    OrderTracker tracker_;
    fms::v1::MissionStatus status_;
};
#endif //FLEETMANAGERSYSTEM_ACTIVEMISSION_HPP
