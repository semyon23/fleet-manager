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
#include "traffic/FixedRouteGate.hpp"

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
    void remove(const std::string& robot_id);                        // dispatch больше не ведёт робота

    // Свободный робот для распределения миссий: IDLE и известно, где он стоит
    struct IdleRobot {
        std::string robot_id;
        std::string map_id;        // pose.mapId
        std::string last_node_id;
    };
    std::vector<IdleRobot> idle_robots() const;

    void set_traffic_registry_ready(bool ready) {
        if (traffic_registry_ready_ == ready) return;
        traffic_registry_ready_ = ready;
        if (on_traffic_epoch) on_traffic_epoch();
    }

    std::function<void()> on_traffic_epoch;
    const std::string& traffic_instance() const { return traffic_instance_; }
    void set_traffic_instance(const std::string& instance);
    std::uint64_t robot_epoch() const { return robot_epoch_; }
    void set_robot_epoch(std::uint64_t epoch) { robot_epoch_ = epoch; }
    bool traffic_inventory_ready() const;
    bool assessment_fleet_ready() const;
    const fms::v1::RobotStatus* live_status(const std::string& robot_id) const;
    const fms::v1::RobotStatus* live_status(const std::string& robot_id, const std::string& instance) const;

    std::vector<traffic::TrafficOccupant> traffic_occupants() const;

    std::function<void()> on_idle;   // робот стал IDLE: Allocator проверяет очередь

    // M-5: журнал и тревоги. before == nullptr — раньше ничего не знали
    std::function<void(const std::string& robot_id, const fms::v1::RobotStatus* before,
                       const fms::v1::RobotStatus& after)> on_status_change;   // не для удаляемых
    std::function<void(const std::string& robot_id, const fms::database::v1::RobotAvailability* before,
                       const fms::database::v1::RobotAvailability& after)> on_availability;
    std::function<void(const std::string& robot_id)> on_robot_removed;            // удаляется или удалён
    std::function<void(const std::set<std::string>& robot_ids)> on_keep_only;      // остальных в базе нет
    std::function<void(bool connected)> on_dispatch;
private:
    struct Robot {
        std::uint64_t traffic_epoch = 0;
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
    std::string traffic_instance_;
    std::uint64_t robot_epoch_ = 0;
    std::uint64_t traffic_epoch_ = 0;
    bool traffic_registry_ready_ = false;
    bool dispatch_connected_ = false;
    std::map<std::string, Robot> robots_;
    std::map<std::string, fms::database::v1::RobotAvailability> current_;   // последнее посчитанное
    std::map<std::string, fms::database::v1::RobotAvailability> pending_;   // ещё не записано в базу
    Signal wakeup_;                     // «в pending_ есть что писать»
    asio::steady_timer retry_timer_;    // пауза, если база недоступна
    bool stopped_ = false;
};

#endif //FLEETMANAGERSYSTEM_AVAILABILITYTRACKER_HPP
