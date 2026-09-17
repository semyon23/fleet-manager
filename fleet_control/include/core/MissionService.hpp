#pragma once
#ifndef MISSIONSERVICE_HPP
#define MISSIONSERVICE_HPP


#include <string>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <include/objects/MissionObject.hpp>

namespace fms {

    class MissionsService {
    public:
        MissionsService() = default;

        MissionsService(const fms::MissionsService&) = delete;
        MissionsService& operator=(const MissionsService&) = delete;

        std::string create_mission(fms::MissionObject mission);

        std::optional<MissionStatus> get_mission_status(const std::string& id) const;

        bool start_mission(const std::string& id, const std::string& robot_name);

        bool complete_mission(const std::string& id);

        bool cancel_mission(const std::string& id);

        bool fail_mission(const std::string& id);

        std::optional<MissionObject> get_next_pending_mission();

        bool update_mission_status(const std::string& id, MissionStatus new_status, const std::string& robot_name = "");

    private:
        std::unordered_map<std::string, MissionObject> missions;
        mutable std::mutex mutex;
    };

} // namespace fms

#endif //MISSIONSERVICE_HPP
