#pragma once

#include "fleet_dispatch.pb.h"
#include "traffic/FixedRouteGate.hpp"

namespace recovery_assessment {

struct LocalEvidence {
    std::string robot;
    std::uint32_t attempt = 0;
    std::vector<traffic::ProtectedRoute> steps;
    bool recovering = true;
};
using Local = std::map<std::string, LocalEvidence>;

// Compare a first-attempt MoveStep with an accepted state from live_status().
bool matches_move(const fms::v1::RobotStatus& status, const std::string& mission,
                  std::size_t step, const fms::v1::MoveProgress& progress);

// Include every local obligation; empty steps mean pending proof. For recovery
// steps come from read_authority for this exact attempt. Empty extend_mission
// previews a new assignment. The caller verifies freshness and candidate state.
traffic::GateResult assess(const Graph& graph,
    const fms::dispatch::v1::GetRecoveryInventoryResponse& inventory,
    std::span<const traffic::TrafficOccupant> occupants,
    std::span<const traffic::ProtectedRoute> held, const Local& local,
    const traffic::ProtectedRoute& wanted, const std::string& extend_mission,
    bool candidate_ready, bool* inventory_mismatch = nullptr);

} // namespace recovery_assessment
