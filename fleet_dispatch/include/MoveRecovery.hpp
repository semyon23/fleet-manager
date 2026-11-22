#ifndef FLEETMANAGERSYSTEM_MOVERECOVERY_HPP
#define FLEETMANAGERSYSTEM_MOVERECOVERY_HPP

#pragma once

#include <optional>
#include <string_view>
#include "fleet_database.pb.h"
#include "vda5050.hpp"

namespace move_recovery {

    enum class Decision { Attach, Wait, Conflict };

    struct Result {
        Decision decision;
        std::string_view reason;
        std::optional<std::uint32_t> last_node_index;
    };

    // request_id принадлежит конкретному запросу текущего запуска и этой сессии.
    Result reconcile(const fms::database::v1::RobotSpec& robot,
                     const fms::database::v1::MissionExecution& head,
                     const fms::database::v1::MissionExecution& grant,
                     std::string_view request_id,
                     const vda5050::State* state);

    bool matches_attached_move(const vda5050::Order& full,
                           const fms::v1::StepStatus& step,
                           const vda5050::State& state);
}  // namespace move_recovery


#endif //FLEETMANAGERSYSTEM_MOVERECOVERY_HPP
