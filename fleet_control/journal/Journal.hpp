#ifndef FLEETMANAGERSYSTEM_JOURNAL_HPP
#define FLEETMANAGERSYSTEM_JOURNAL_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <deque>
#include <string>

#include "journal/RobotEvents.hpp"
#include "services/Services.hpp"

// Журнал (M-5): события роботов и системы — в базу пачками (AppendEvents), по порядку.
// События миссий пишет MissionStore вместе с миссией, тревог — AlarmEngine вместе с тревогой.
// База недоступна — копим и повторяем; копится слишком много — старые выбрасываем (журнал не важнее работы).
class Journal {
public:
    Journal(asio::io_context& io_context, Services& services);

    void start();
    void stop();

    // source — ROBOT | SYSTEM; robot_id / mission_id — "" если не о них. Время — сейчас
    void add(const std::string& source, const JournalEvent& event, const std::string& robot_id = "",
             const std::string& mission_id = "");

private:
    asio::awaitable<void> write_loop();

    using Signal = asio::experimental::channel<void(boost::system::error_code)>;

    asio::io_context& io_context_;
    Services& services_;
    std::deque<fms::database::v1::Event> pending_;   // ещё не записано, по порядку
    Signal wakeup_;
    asio::steady_timer retry_timer_;
    bool stopped_ = false;
};

#endif //FLEETMANAGERSYSTEM_JOURNAL_HPP