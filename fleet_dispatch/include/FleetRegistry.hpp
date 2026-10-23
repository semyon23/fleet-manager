#ifndef FLEETREGISTRY_HPP
#define FLEETREGISTRY_HPP

#pragma once

#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include "RobotMessages.hpp"
#include "RobotSession.hpp"

enum class AddResult {
    Added,
    AlreadyExists,
    Conflict,
};

class FleetRegistry {
public:
    FleetRegistry(asio::io_context& io_context, Outbox& outbox, RobotStatusHub& status_hub);

    AddResult add(const std::string& robot_id, const std::string& manufacturer, const std::string& serial_number);
    bool remove(const std::string& robot_id);
    void stop_all();

    RobotSession* find(const std::string& robot_id);
    RobotSession* find_by_mission(const std::string& mission_id);
    bool post(const std::string& manufacturer, const std::string& serial_number, VdaMessage message);

private:
    RobotSession* find_by_vda(const std::string& manufacturer, const std::string& serial_number);

    asio::io_context& io_context_;
    Outbox& outbox_;
    RobotStatusHub& status_hub_;
    std::unordered_map<std::string, std::shared_ptr<RobotSession>> sessions_;
};


#endif //FLEETREGISTRY_HPP
