#ifndef FLEETMANAGERSYSTEM_MISSIONASSIGNER_HPP
#define FLEETMANAGERSYSTEM_MISSIONASSIGNER_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <boost/asio/experimental/channel.hpp>
#include "fleet/AvailabilityTracker.hpp"
#include "maps/MapStore.hpp"
#include "missions/MissionStore.hpp"


class MissionAssigner {
public:
    MissionAssigner(asio::io_context& io_context, MissionStore& missions, AvailabilityTracker& availability,
                    MapStore& maps);

    void start();
    void stop();

    // Что-то изменилось: новая миссия, робот освободился, карта сохранена.
    // Сигнал лежит в канале, пока цикл его не заберёт: много событий подряд — один проход
    void wake();

    // Миссия назначена роботу (ASSIGNED) — MissionRunner строит маршруты и отправляет её в dispatch
    std::function<void(Mission&, const std::string& start_node)> on_assigned;
private:
    void assign();
    asio::awaitable<void> loop();

    using Signal = asio::experimental::channel<void(boost::system::error_code)>;

    asio::io_context& io_context_;
    MissionStore& missions_;
    AvailabilityTracker& availability_;
    MapStore& maps_;
    Signal wakeup_;                 // «что-то изменилось»: один сигнал на много событий
    asio::steady_timer timer_;      // страховочный проход раз в 5 с
    bool stopped_ = false;
};
#endif //FLEETMANAGERSYSTEM_MISSIONASSIGNER_HPP