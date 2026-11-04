#ifndef FLEETMANAGERSYSTEM_AVAILABILITYTRACKER_HPP
#define FLEETMANAGERSYSTEM_AVAILABILITYTRACKER_HPP
#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include "maps/MapStore.hpp"
#include "services/Services.hpp"


class AvailabilityTracker {
public:
    AvailabilityTracker(asio::io_context& io_context, Services& services, const MapStore& maps, double low_battery_percent);

    void start();
    void stop();

    void on_status(const fms::v1::RobotStatus& status);              // стрим dispatch: свежий статус
    void on_robot(const fms::database::v1::RobotRecord& record);     // робот из базы: регистрация, сверка
    void set_deleting(const std::string& robot_id);                  // DELETE принят
    void set_dispatch_connected(bool connected);                     // нет dispatch — все роботы OFFLINE
    void recompute_all();
    void keep_only(const std::set<std::string>& robot_ids);          // остальных в базе уже нет

private:
    struct Robot {
        bool deleting = false;
        std::optional<fms::v1::RobotStatus> status;   // последний известный
        bool status_from_stream = false;              // из стрима свежее, чем из базы, — базой не перезаписываем
    };

    void update(const std::string& robot_id);         // пересчитать; изменилась — в очередь записи
    asio::awaitable<void> write_loop();

    using Signal = asio::experimental::channel<void(boost::system::error_code)>;

    asio::io_context& io_context_;
    Services& services_;
    const MapStore& maps_;
    double low_battery_percent_;
    bool dispatch_connected_ = false;
    std::map<std::string, Robot> robots_;
    std::map<std::string, fms::database::v1::RobotAvailability> current_;   // последнее посчитанное
    std::map<std::string, fms::database::v1::RobotAvailability> pending_;   // ещё не записано в базу
    Signal wakeup_;                     // «в pending_ есть что писать»
    asio::steady_timer retry_timer_;    // пауза, если база недоступна
    bool stopped_ = false;
};

#endif //FLEETMANAGERSYSTEM_AVAILABILITYTRACKER_HPP
