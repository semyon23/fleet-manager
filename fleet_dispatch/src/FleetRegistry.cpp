#include "../include/FleetRegistry.hpp"
#include "spdlog/spdlog.h"
#include "VdaTopics.hpp"

FleetRegistry::FleetRegistry(asio::io_context& io_context, Outbox& outbox)
    : io_context_(io_context), outbox_(outbox) {}

AddResult FleetRegistry::add(const std::string& robot_id, const std::string& manufacturer,
                             const std::string& serial_number) {
    if (RobotSession* existing = find(robot_id)) {
        if (existing->manufacturer() == manufacturer && existing->serial_number() == serial_number) {
            return AddResult::AlreadyExists;
        }
        return AddResult::Conflict;
    }
    if (find_by_vda(manufacturer, serial_number)) {
        return AddResult::Conflict;   // этот физический робот уже зарегистрирован под другим id
    }

    auto session = std::make_shared<RobotSession>(io_context_, outbox_, robot_id, manufacturer, serial_number);
    sessions_[robot_id] = session;
    session->start();
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