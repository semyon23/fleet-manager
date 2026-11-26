#ifndef FLEETREGISTRY_HPP
#define FLEETREGISTRY_HPP

#pragma once

#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include "RobotMessages.hpp"
#include "RobotSession.hpp"
#include "StartupInventory.hpp"
#include <optional>
#include "ProbeQueue.hpp"
#include "RecoveryProbe.hpp"
#include "RecoveryInventory.hpp"

enum class AddResult {
    Added,
    AlreadyExists,
    Conflict,
};

class FleetRegistry {
public:
    FleetRegistry(asio::io_context& io_context, Outbox& outbox, RobotStatusHub& status_hub, MissionStatusHub& mission_hub,
                  DatabaseWriter& database);

    AddResult add(const std::string& robot_id, const std::string& manufacturer, const std::string& serial_number);
    asio::awaitable<void> restore(StartupInventory inventory);
    asio::awaitable<grpc::Status> recovery_inventory(fms::dispatch::v1::GetRecoveryInventoryResponse& out);
    void reject_startup() { recovery_.reject_startup(); }
    const std::string& instance_id() const { return recovery_.snapshot().instance_id(); }

    const StartupInventory* inventory() const {
        return inventory_ ? &*inventory_ : nullptr;
    }
    bool remove(const std::string& robot_id);
    void stop_all();

    RobotSession* find(const std::string& robot_id);
    RobotSession* find_by_mission(const std::string& mission_id);
    bool post(const std::string& manufacturer, const std::string& serial_number, VdaMessage message);

private:
    struct RecoveryRead {
        explicit RecoveryRead(asio::io_context& io)
            : done(io, asio::steady_timer::time_point::max()) {}
        asio::steady_timer done;
        grpc::Status status{grpc::StatusCode::UNAVAILABLE, "recovery read interrupted"};
        fms::dispatch::v1::GetRecoveryInventoryResponse reply;
    };
    RecoveryInventory recovery_;
    std::shared_ptr<RecoveryRead> reading_;
    std::map<std::string, RecoveryRobotView> recovery_robots() const;

    void start_probes();
    void pump_probes();
    bool observe(const std::string& id, const vda5050::State* state);

    ProbeQueue queue_;
    std::map<std::string, RecoveryProbe> probes_;
    bool timer_armed_ = false;
    RobotSession* find_by_vda(const std::string& manufacturer, const std::string& serial_number);

    asio::io_context& io_context_;
    asio::steady_timer probe_timer_{io_context_};
    Outbox& outbox_;
    RobotStatusHub& status_hub_;
    MissionStatusHub& mission_hub_;
    std::unordered_map<std::string, std::shared_ptr<RobotSession>> sessions_;
    DatabaseWriter& database_;
    std::optional<StartupInventory> inventory_;
    bool stopping_ = false;
};


#endif //FLEETREGISTRY_HPP
