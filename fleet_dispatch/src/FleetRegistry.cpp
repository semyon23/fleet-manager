#include "../include/FleetRegistry.hpp"
#include "spdlog/spdlog.h"
#include "../include/VdaTopics.hpp"

FleetRegistry::FleetRegistry(asio::io_context& io_context, Outbox& outbox, RobotStatusHub& status_hub, MissionStatusHub& mission_hub,
                  DatabaseWriter& database)
    : io_context_(io_context), outbox_(outbox),  status_hub_(status_hub), mission_hub_(mission_hub), database_(database)  {}

AddResult FleetRegistry::add(const std::string& robot_id, const std::string& manufacturer,
                             const std::string& serial_number) {
    if (RobotSession* existing = find(robot_id)) {
        if (existing->manufacturer() == manufacturer && existing->serial_number() == serial_number) {
            return AddResult::AlreadyExists;
        }
        return AddResult::Conflict;
    }
    if (find_by_vda(manufacturer, serial_number)) {
        return AddResult::Conflict;
    }
    auto session = std::make_shared<RobotSession>(io_context_, outbox_, status_hub_, mission_hub_, database_, robot_id, manufacturer, serial_number);
    sessions_[robot_id] = session;
    session->start();
    spdlog::info("[registry] robot {} added, total {}", robot_id, sessions_.size());
    return AddResult::Added;
}

asio::awaitable<void> FleetRegistry::restore(StartupInventory inventory) {
    for (const auto& record : inventory.snapshot.robots()) {
        if (stopping_) co_return;

        const auto& s = record.spec();
        auto session = std::make_shared<RobotSession>(
            io_context_, outbox_, status_hub_, mission_hub_, database_,
            s.robot_id(), s.manufacturer(), s.serial_number());

        sessions_.emplace(s.robot_id(), session);

        std::string mission;
        auto it = inventory.occupied.find(s.robot_id());
        if (it != inventory.occupied.end()) mission = it->second;

        co_await session->start_restored(record, std::move(mission));
    }

    if (!stopping_) inventory_.emplace(std::move(inventory));
}

bool FleetRegistry::remove(const std::string& robot_id) {
    auto it = sessions_.find(robot_id);
    if (it == sessions_.end()) {
        return false;
    }
    it->second->abort_mission("robot " + robot_id + " was removed");   // 6
    it->second->stop();
    sessions_.erase(it);

    if (inventory_) inventory_->deleting.erase(robot_id);

    status_hub_.remove(robot_id);
    spdlog::info("[registry] robot {} removed, total {}", robot_id, sessions_.size());
    return true;
}

void FleetRegistry::stop_all() {
    stopping_ = true;
    for (auto& [id, session] : sessions_) {
        session->stop();
    }
    sessions_.clear();
}

RobotSession* FleetRegistry::find(const std::string& robot_id) {
    auto it = sessions_.find(robot_id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

RobotSession* FleetRegistry::find_by_mission(const std::string& mission_id) {
    for (auto& [id, session] : sessions_) {
        if (session->mission() && session->mission()->mission_id() == mission_id) {
            return session.get();
        }
    }
    return nullptr;
}

bool FleetRegistry::post(const std::string& manufacturer, const std::string& serial_number, VdaMessage message) {
    RobotSession* session = find_by_vda(manufacturer, serial_number);
    if (!session) {
        return false;
    }
    return session->post(std::move(message));
}

RobotSession* FleetRegistry::find_by_vda(const std::string& manufacturer, const std::string& serial_number) {
    for (auto& [id, session] : sessions_) {
        if (session->manufacturer() == manufacturer && session->serial_number() == serial_number) {
            return session.get();
        }
    }
    return nullptr;
}