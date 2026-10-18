
#ifndef ROBOTSESSION_HPP
#define ROBOTSESSION_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>

#include "fms_types.pb.h"

namespace asio = boost::asio;

struct RobotMessage {
    std::string topic;
    std::string payload;
};

class RobotSession : public std::enable_shared_from_this<RobotSession>{
public:
    RobotSession(asio::io_context& io_context, std::string robot_id,
                 std::string manufacturer, std::string serial_number);

    void start();
    void stop();

    bool post(RobotMessage message);

    const std::string& robot_id() const { return robot_id_; }
    const std::string& manufacturer() const { return manufacturer_; }
    const std::string& serial_number() const { return serial_number_; }
    const fms::v1::RobotStatus& status() const { return status_; }

private:
    asio::awaitable<void> run();

    using Inbox = asio::experimental::channel<void(boost::system::error_code, RobotMessage)>;

    asio::io_context& io_context_;
    std::string robot_id_;
    std::string manufacturer_;
    std::string serial_number_;
    Inbox inbox_;
    fms::v1::RobotStatus status_;

};

#endif //ROBOTSESSION_HPP
