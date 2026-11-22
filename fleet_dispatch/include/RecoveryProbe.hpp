#ifndef FLEETMANAGERSYSTEM_RECOVERYPROBE_HPP
#define FLEETMANAGERSYSTEM_RECOVERYPROBE_HPP

#pragma once

#include "MoveRecovery.hpp"
#include <optional>
#include <string>

struct RecoveryProbe {

    using Execution = fms::database::v1::MissionExecution;

    const fms::database::v1::RobotSpec* robot = nullptr;
    const Execution* head = nullptr;
    const Execution* grant = nullptr;
    std::string token;
    std::optional<move_recovery::Result> decision;

    bool acknowledged(const vda5050::State& state) const {
        if (token.empty() ||
            state.header.manufacturer != robot->manufacturer() ||
            state.header.serial_number != robot->serial_number()) {
            return false;
            }

        const vda5050::ActionState* found = nullptr;
        for (const auto& a : state.instant_action_states) {
            if (a.action_id != token) continue;
            if (found) return false;
            found = &a;
        }

        return found &&
               found->action_status == vda5050::ActionStatus::Finished &&
               (!found->action_type || *found->action_type == "stateRequest");
    }

    void reset() {
        token.clear();
        decision.reset();
    }
};

#endif //FLEETMANAGERSYSTEM_RECOVERYPROBE_HPP
