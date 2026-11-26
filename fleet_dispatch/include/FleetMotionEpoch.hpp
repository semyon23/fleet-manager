#pragma once

#include "fms_types.pb.h"
#include <cstdint>
#include <string>

// All status publications and outgoing grants share the dispatch executor.
// Keep the interval over which only the most recently changed owner changed.
class FleetMotionEpoch {
public:
    std::uint64_t current() const { return current_; }
    void change(const std::string& robot) {
        if (robot != last_) {
            last_ = robot;
            since_ = current_;
        }
        ++current_;
    }
    bool peers_unchanged(std::uint64_t from, const std::string& target) const {
        return from && from <= current_ &&
            (from == current_ || (last_ == target && from >= since_));
    }
    static bool changed(const fms::v1::RobotStatus& a, const fms::v1::RobotStatus& b) {
        if (a.mission_id() != b.mission_id() || a.connection() != b.connection() ||
            a.paused() != b.paused() || a.operating_mode() != b.operating_mode() ||
            a.safety().field_violation() != b.safety().field_violation() ||
            a.safety().emergency_stop() != b.safety().emergency_stop() ||
            a.localized() != b.localized() || a.has_pose() != b.has_pose() || a.pose().map_id() != b.pose().map_id() ||
            a.has_observed_order() != b.has_observed_order() || a.driving() != b.driving() ||
            a.observed_order().idle() != b.observed_order().idle() ||
            a.observed_order().order_id() != b.observed_order().order_id() ||
            a.observed_order().update_id() != b.observed_order().update_id()) return true;
        // An active owner's issued base stays reserved as it advances. An idle
        // owner's actual node and pose are what justify its current occupancy.
        return (b.mission_id().empty() || b.observed_order().idle()) && (a.last_node_id() != b.last_node_id() ||
            a.pose().x() != b.pose().x() || a.pose().y() != b.pose().y());
    }
private:
    std::uint64_t current_ = 1;
    std::uint64_t since_ = 1;
    std::string last_;
};
