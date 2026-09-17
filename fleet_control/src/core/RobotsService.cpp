
#include "include/core/RobotsService.hpp"

bool fms::RobotsService::add_robot(const fms::RobotObject& robot){
    std::unique_lock<std::shared_mutex> lock(rw_mutex);

    if (robots.contains(robot.name)) return false;
    robots[robot.name] = robot;
    return true;
}

bool fms::RobotsService::remove_robot(const std::string& name){
    std::unique_lock<std::shared_mutex> lock(rw_mutex);
    return robots.erase(name) > 0;
}

std::optional<fms::RobotObject> fms::RobotsService::get_robot(const std::string& name) const{
    std::shared_lock<std::shared_mutex> lock(rw_mutex);

    auto it = robots.find(name);
    if (it != robots.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<std::string> fms::RobotsService::get_all_robot_names() const{
    std::shared_lock<std::shared_mutex> lock(rw_mutex);

    std::vector<std::string> names;
    names.reserve(robots.size());

    for (const auto& [name, _] : robots) {
        names.push_back(name);
    }
    return names;
}
std::vector<std::string> fms::RobotsService::get_available_robots(const std::string& required_class) const{
    std::shared_lock<std::shared_mutex> lock(rw_mutex);

    std::vector<std::string> available;
    for (const auto& [name, robot] : robots) {
        if (robot.status.online &&
            robot.status.state == RobotState::IDLE &&
            robot.status.identifier.get_agv_class() == required_class) {
            available.push_back(name);
            }
    }
    return available;
}

bool fms::RobotsService::update_robot(const std::string& name, std::function<void(fms::RobotObject&)> modifier){
    std::unique_lock<std::shared_mutex> lock(rw_mutex);
    auto it = robots.find(name);
    if (it == robots.end()) {
        return false;
    }
    modifier(it->second);
    return true;
}
