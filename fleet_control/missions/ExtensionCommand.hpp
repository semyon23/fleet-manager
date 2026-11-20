#ifndef FLEETMANAGERSYSTEM_EXTENSIONCOMMAND_HPP
#define FLEETMANAGERSYSTEM_EXTENSIONCOMMAND_HPP

#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "fleet_dispatch.pb.h"

// Управление одной неизменяемой командой. Один поток.
// Состояние живёт в памяти control; восстановление после рестарта отдельно.
class ExtensionCommand {
public:
    enum class State {
        DispatchPending,
        RobotPending,
        Confirmed,
        Blocked
    };

    ExtensionCommand(fms::dispatch::v1::ExtendMissionRequest request, const fms::v1::MoveProgress& previous);


    const fms::dispatch::v1::ExtendMissionRequest& request() const;
    State state() const;
    const std::string& reason() const;
    void block(std::string reason);
    void observe(const fms::v1::MoveProgress& progress);


private:
    fms::dispatch::v1::ExtendMissionRequest request_;
    std::uint32_t total_nodes_ = 0;
    State state_ = State::DispatchPending;
    std::string reason_;
};

#endif //FLEETMANAGERSYSTEM_EXTENSIONCOMMAND_HPP
