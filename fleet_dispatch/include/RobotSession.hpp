
#ifndef ROBOTSESSION_HPP
#define ROBOTSESSION_HPP

#pragma once
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <string>

#include "RobotMessages.hpp"
#include "RobotStatusHub.hpp"
#include "fms_types.pb.h"
#include "vda5050.hpp"


class RobotSession : public std::enable_shared_from_this<RobotSession> {
public:
    RobotSession(asio::io_context& io_context, Outbox& outbox, RobotStatusHub& status_hub, std::string robot_id,
                 std::string manufacturer, std::string serial_number);

    void start();
    void stop();

    bool pause();
    bool resume();
    bool initialize_position(const fms::v1::Pose& pose, const std::string& last_node_id);
    bool post(VdaMessage message);

    const std::string& robot_id() const { return robot_id_; }
    const std::string& manufacturer() const { return manufacturer_; }
    const std::string& serial_number() const { return serial_number_; }
    const fms::v1::RobotStatus& status() const { return status_; }

private:
    asio::awaitable<void> run();
    asio::awaitable<void> watchdog();

    void handle(const VdaMessage& message);
    void count_invalid_message(const std::string& topic, const std::string& error);
    void log_status() const;

    bool send(const std::string& kind, std::string payload);
    bool request(MqttAction action, const std::string& topic, std::string payload = {});

    bool send_instant_action(const std::string& action_type, std::vector<vda5050::ActionParameter> parameters);
    void check_instant_actions(const vda5050::State& state);

    using Inbox = asio::experimental::channel<void(boost::system::error_code, VdaMessage)>;

    asio::io_context& io_context_;

    Outbox& outbox_;
    RobotStatusHub& status_hub_;
    std::string robot_id_;
    std::string manufacturer_;
    std::string serial_number_;
    std::string state_topic_;
    std::string connection_topic_;
    std::string factsheet_topic_;
    Inbox inbox_;
    fms::v1::RobotStatus status_;
    std::optional<vda5050::Factsheet> factsheet_;

    asio::steady_timer watchdog_timer_;
    std::chrono::steady_clock::time_point last_state_time_;
    std::uint32_t instant_actions_header_id_;
    std::unordered_map<std::string, std::string> pending_actions_;
    bool stopped_ = false;
};
#endif //ROBOTSESSION_HPP
