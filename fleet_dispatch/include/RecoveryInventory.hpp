
#ifndef FLEETMANAGERSYSTEM_RECOVERYINVENTORY_HPP
#define FLEETMANAGERSYSTEM_RECOVERYINVENTORY_HPP

#pragma once

#include "fleet_database.pb.h"
#include "fleet_dispatch.pb.h"
#include <google/protobuf/util/message_differencer.h>
#include <algorithm>
#include <chrono>
#include <optional>
#include <map>
#include <set>
#include <string>

struct RecoveryRobotView {
    std::string mission;
    bool blocked = false;
    bool conflict = false;
};

class RecoveryInventory {
    using Reply = fms::dispatch::v1::GetRecoveryInventoryResponse;
    using Rows = fms::database::v1::RecoveryRows;

public:
    using Clock = std::chrono::steady_clock;
    explicit RecoveryInventory(const std::string& instance);
    const Reply& snapshot() const { return view_; }
    void reject_startup();
    void update(const Rows& rows, const std::map<std::string, RecoveryRobotView>& live,
                bool startup_consistent, Clock::time_point read_started = Clock::now());
    void read_failed() { expires_.reset(); }
    bool identifies_active(const fms::dispatch::v1::GetMissionRequest& request,
                           Clock::time_point now) const;

private:
    Reply view_;
    std::optional<Clock::time_point> expires_;
};

#endif //FLEETMANAGERSYSTEM_RECOVERYINVENTORY_HPP
