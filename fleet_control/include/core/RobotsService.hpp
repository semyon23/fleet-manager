#pragma once
#ifndef ROBOTSERVICE_HPP
#define ROBOTSERVICE_HPP

#include <string>
#include <vector>
#include <optional>
#include "include/objects/RobotObject.hpp"
#include <shared_mutex>
#include <mutex>

namespace fms
{
    class RobotsService {
    public:
        RobotsService() = default;
        RobotsService(const RobotsService&) = delete;
        RobotsService& operator=(const RobotsService&) = delete;


        bool add_robot(const fms::RobotObject& robot);

        bool remove_robot(const std::string& name);

        std::optional<fms::RobotObject> get_robot(const std::string& name) const;

        std::vector<std::string> get_all_robot_names() const;

        bool update_robot(const std::string& name, std::function<void(fms::RobotObject&)> modifier);

        std::vector<std::string> get_available_robots(const std::string& required_class) const;

    private:

        std::unordered_map<std::string, fms::RobotObject> robots;

        // Мьютекс для разделяемого чтения и эксклюзивной записи (C++17)
        // Позволяет множеству потоков одновременно читать данные роботов (GET-запросы),
        // но блокирует всё, когда происходит запись (POST/DELETE или обновление телеметрии)
        mutable std::shared_mutex rw_mutex;
    };
}


#endif //ROBOTSERVICE_HPP
