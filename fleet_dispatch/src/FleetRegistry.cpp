#include "../include/FleetRegistry.hpp"
#include "spdlog/spdlog.h"

FleetRegistry::FleetRegistry(asio::io_context& io_context) : io_context_(io_context) {}

AddResult FleetRegistry::add(const std::string& robot_id, const std::string& manufacturer, const std::string& serial_number) {

    if (RobotSession* existing = find(robot_id)) {
        if (existing->manufacturer() == manufacturer && existing->serial_number() == serial_number) {
            return AddResult::AlreadyExists;
        }
        return AddResult::Conflict;
    }
    if (find_by_vda(manufacturer, serial_number)) {
        return AddResult::Conflict;
    }

    auto session = std::make_shared<RobotSession>(io_context_, robot_id, manufacturer, serial_number);
    session->start();
    sessions_[robot_id] = std::move(session);
    spdlog::info("[registry] robot {} added, total {}", robot_id, sessions_.size());
    return AddResult::Added;
}

bool FleetRegistry::remove(const std::string& robot_id) {
    auto it = sessions_.find(robot_id);
    if (it == sessions_.end()) {
        return false;
    }
    it->second->stop();
    sessions_.erase(it);
    spdlog::info("[registry] robot {} removed, total {}", robot_id, sessions_.size());
    return true;
}

void FleetRegistry::stop_all() {
    for (auto& [id, session] : sessions_) {
        session->stop();
    }
    sessions_.clear();
}

RobotSession* FleetRegistry::find(const std::string& robot_id) {
    auto it = sessions_.find(robot_id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

RobotSession* FleetRegistry::find_by_vda(const std::string& manufacturer, const std::string& serial_number) {
    for (auto& [id, session] : sessions_) {
        if (session->manufacturer() == manufacturer && session->serial_number() == serial_number) {
            return session.get();
        }
    }
    return nullptr;
}