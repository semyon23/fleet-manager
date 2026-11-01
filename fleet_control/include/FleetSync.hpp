#ifndef FLEETMANAGERSYSTEM_FLEETSYNC_HPP
#define FLEETMANAGERSYSTEM_FLEETSYNC_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <set>
#include <string>

#include "Services.hpp"

class FleetSync {
public:
    FleetSync(asio::io_context& io_context, Services& services);

    void start();
    void stop();

    asio::awaitable<void> add_to_dispatch(const fms::database::v1::RobotSpec& spec);
    asio::awaitable<void> finish_delete(const std::string& robot_id);

private:
    using StatusStreamRPC = agrpc::ClientRPC<&fms::dispatch::v1::FleetDispatch::Stub::PrepareAsyncStreamRobotStatus>;

    asio::awaitable<void> watch_dispatch();   // стрим статусов, переподключение раз в 2 с
    asio::awaitable<void> sync_loop();        // сверка раз в 10 с или сразу после подключения
    asio::awaitable<void> sync();
    void on_update(const fms::dispatch::v1::RobotStatusUpdate& update);

    asio::io_context& io_context_;
    Services& services_;
    std::set<std::string> in_dispatch_;   // кого dispatch ведёт — по стриму
    bool connected_ = false;              // стрим открыт и снимок получен
    bool database_ok_ = true;             // для лога: база пропала / вернулась
    StatusStreamRPC* stream_ = nullptr;   // текущий стрим — stop() его прерывает
    asio::steady_timer reconnect_timer_;
    asio::steady_timer sync_timer_;
    bool stopped_ = false;
};

#endif //FLEETMANAGERSYSTEM_FLEETSYNC_HPP
