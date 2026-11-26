#ifndef ROBOTSTATUSHUB_HPP
#define ROBOTSTATUSHUB_HPP

#pragma once

#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "fleet_dispatch.pb.h"
#include "FleetMotionEpoch.hpp"
#include "fms_types.pb.h"

namespace asio = boost::asio;
namespace dv1 = fms::dispatch::v1;



class RobotStatusHub {
public:
    using Channel = asio::experimental::channel<void(boost::system::error_code, dv1::RobotStatusUpdate)>;

    struct Subscriber {
        Subscriber(asio::io_context& io_context, std::vector<std::string> robot_ids);
        bool wants(const std::string& robot_id) const;
        std::vector<std::string> robot_ids;
        Channel channel;
    };

    explicit RobotStatusHub(asio::io_context& io_context);

    void publish(const fms::v1::RobotStatus& status, bool traffic_changed = false);
    const FleetMotionEpoch& motion_epoch() const { return motion_epoch_; }
    void remove(const std::string& robot_id);

    std::shared_ptr<Subscriber> subscribe(std::vector<std::string> robot_ids);
    void unsubscribe(const std::shared_ptr<Subscriber>& subscriber);

    void close_all();

private:
    void send_to_all(const std::string& robot_id, const dv1::RobotStatusUpdate& update);

    FleetMotionEpoch motion_epoch_;
    asio::io_context& io_context_;
    std::unordered_map<std::string, fms::v1::RobotStatus> last_status_;
    std::vector<std::shared_ptr<Subscriber>> subscribers_;
};


#endif //ROBOTSTATUSHUB_HPP
