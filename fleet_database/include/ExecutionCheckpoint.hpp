
#ifndef FLEETMANAGERSYSTEM_EXECUTIONCHECKPOINT_HPP
#define FLEETMANAGERSYSTEM_EXECUTIONCHECKPOINT_HPP

#pragma once

#include "fleet_database.pb.h"

namespace execution_checkpoint {

    inline bool confirmed_cancel(const fms::database::v1::MissionExecution& e) {
        return e.status().state() == fms::v1::MISSION_STATE_CANCELED &&
               !e.cancel_action_id().empty() && e.stopped_after_cancel();
    }

    inline fms::v1::GetExecutionCheckpointResponse::Kind classify(
        const fms::database::v1::MissionExecution& e, bool terminal, bool uncertain) {
        using namespace fms::v1;
        using Result = GetExecutionCheckpointResponse;
        const auto& s = e.status();
        if (uncertain) return Result::UNCERTAIN;

        if (terminal) {
            if (e.has_order() || !s.current_node_id().empty()) return Result::UNCERTAIN;
            if (s.state() == MISSION_STATE_COMPLETED || confirmed_cancel(e))
                return Result::CONFIRMED_TERMINAL;
            return Result::UNCERTAIN;
        }

        if (s.current_node_id().empty()) return Result::UNCERTAIN;
        switch (s.state()) {
            case MISSION_STATE_RUNNING:
            case MISSION_STATE_PAUSED:
                return Result::ACTIVE;
            case MISSION_STATE_CANCELING:
                return e.cancel_action_id().empty() ? Result::UNCERTAIN : Result::ACTIVE;
            default:
                return Result::UNCERTAIN;
        }
    }

}  // namespace execution_checkpoint

#endif //FLEETMANAGERSYSTEM_EXECUTIONCHECKPOINT_HPP
