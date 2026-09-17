
#include "include/core/MissionService.hpp"

std::string fms::MissionsService::create_mission(fms::MissionObject mission) {
    std::unique_lock<std::mutex> lock(mutex);

    if (mission.id.empty()) {
        boost::uuids::random_generator gen;
        boost::uuids::uuid u = gen();
        mission.id = boost::uuids::to_string(u);
    }

    mission.status = MissionStatus::PENDING;
    std::string allocated_id = mission.id;

    missions[allocated_id] = std::move(mission);

    return allocated_id;
}

std::optional<fms::MissionStatus> fms::MissionsService::get_mission_status(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = missions.find(id);
    if (it != missions.end()) {
        return it->second.status;
    }
    return std::nullopt;
}

bool fms::MissionsService::start_mission(const std::string& id, const std::string& robot_name){
    return update_mission_status(id, MissionStatus::EXECUTING, robot_name);
}


bool fms::MissionsService::complete_mission(const std::string& id) {
    return update_mission_status(id, MissionStatus::COMPLETED);
}

bool fms::MissionsService::cancel_mission(const std::string& id) {
    return update_mission_status(id, MissionStatus::CANCELLED);
}

bool fms::MissionsService::fail_mission(const std::string& id) {
    return update_mission_status(id, MissionStatus::FAILED);
}

std::optional<fms::MissionObject> fms::MissionsService::get_next_pending_mission() {
    std::unique_lock<std::mutex> lock(mutex);

    for (const auto& [id, mission] : missions) {
        if (mission.status == MissionStatus::PENDING) {
            return mission;
        }
    }
    return std::nullopt;
}

bool fms::MissionsService::update_mission_status(const std::string& id, MissionStatus new_status, const std::string& robot_name) {
    std::unique_lock<std::mutex> lock(mutex);

    auto it = missions.find(id);
    if (it == missions.end()) {
        return false;
    }

    it->second.status = new_status;
    if (!robot_name.empty()) {
        it->second.assigned_robot = robot_name;
    }
    return true;
}