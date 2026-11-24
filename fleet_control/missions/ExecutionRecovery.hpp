#pragma once

#include "fleet_database.pb.h"
#include "fleet_dispatch.pb.h"
#include "missions/Mission.hpp"
#include "traffic/FixedRouteGate.hpp"
#include <optional>
#include <span>

namespace execution_recovery {

using Command = fms::database::v1::ControlCommandRecord;
using Checkpoint = fms::v1::GetExecutionCheckpointResponse;

struct Grant {
    traffic::ProtectedRoute base;
    std::uint32_t revision = 0;
};

struct Authority {
    fms::dispatch::v1::AddMissionRequest add;
    std::vector<Grant> steps;
    bool cancel = false;
};

// Exact Add and every permitted extension of that attempt, checked against this graph.
std::optional<Authority> read_authority(const Mission& mission, const Graph& graph,
                                       std::span<const Command> records, std::uint32_t attempt);

bool matches_execution(const Authority& authority, const Checkpoint& checkpoint);

struct Decision {
    bool terminal = false;
    bool preceding_steps_complete = false;
    std::optional<traffic::ProtectedRoute> held;
    struct Stop {
        std::string node;
        std::string order;
        std::uint32_t update;
        std::uint32_t index;
    };
    std::optional<Stop> expected_stop;
    bool stopped_on_node(const Graph& graph, const fms::v1::RobotStatus* robot) const;
};

Decision terminal(const Authority& authority, const Graph& graph, const Checkpoint& checkpoint);

} // namespace execution_recovery
