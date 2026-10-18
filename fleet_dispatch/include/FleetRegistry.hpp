#ifndef FLEETREGISTRY_HPP
#define FLEETREGISTRY_HPP

#pragma once

#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include "RobotSession.hpp"

enum class AddResult {
    Added,
    AlreadyExists,
    Conflict,
};

class FleetRegistry {
public:
    explicit FleetRegistry(asio::io_context& io_context);

    AddResult add(const std::string& robot_id, const std::string& manufacturer, const std::string& serial_number);
    bool remove(const std::string& robot_id);
    void stop_all();

    RobotSession* find(const std::string& robot_id);
    RobotSession* find_by_vda(const std::string& manufacturer, const std::string& serial_number);

private:
    asio::io_context& io_context_;
    std::unordered_map<std::string, std::shared_ptr<RobotSession>> sessions_;
};


#endif //FLEETREGISTRY_HPP
