#pragma once

#include "fleet_database.pb.h"
#include "fleet_dispatch.pb.h"
#include "missions/Mission.hpp"
#include "missions/RouteParts.hpp"
#include "traffic/FixedRouteGate.hpp"
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace execution_recovery {

using Command = fms::database::v1::ControlCommandRecord;
using Checkpoint = fms::v1::GetExecutionCheckpointResponse;

// Вызывается после подтверждённого закрытия журнала и передачи резерва.
template<class Job>
void finish_checkpoint_job(std::map<std::string, Job>& pending,
                           const std::string& mission_id,
                           const std::function<void()>& on_drained) {
    if (pending.erase(mission_id) != 0 && pending.empty() && on_drained) {
        // Планировщик увидит пустой список сверок в момент сигнала.
        on_drained();
    }
}

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

struct ObservedMove {
    std::uint32_t add_attempt;
    int step;
    fms::v1::MoveProgress progress;
    RouteParts parts;
};

// cp was checked against the local Add/Extend journal by matches_execution().
std::optional<ObservedMove> observe_move(const Checkpoint& cp,
    const fms::dispatch::v1::GetMissionRequest& request,
    const fms::dispatch::v1::GetMissionResponse& reply, const fms::v1::RobotStatus& robot,
    const ObservedMove* previous);

// Recover the robot's *preceding* progress only for the exact committed but
// unpublished extension retained by this same dispatch session.
std::optional<ObservedMove> observe_unpublished(const Checkpoint& cp,
    const fms::dispatch::v1::GetMissionRequest& request, const fms::dispatch::v1::GetMissionResponse& reply,
    const fms::v1::RobotStatus& robot, const ObservedMove* previous,
    const fms::dispatch::v1::ExtendMissionRequest& pending, std::span<const Command> journal);

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
