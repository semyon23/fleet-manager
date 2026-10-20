
#include "../include/RobotStatusHub.hpp"
#include <algorithm>

#include "spdlog/spdlog.h"

namespace {
constexpr std::size_t kChannelCapacity = 1000;
}

RobotStatusHub::Subscriber::Subscriber(asio::io_context& io_context, std::vector<std::string> robot_ids)
    : robot_ids(std::move(robot_ids)), channel(io_context, kChannelCapacity) {}

bool RobotStatusHub::Subscriber::wants(const std::string& robot_id) const {
    return robot_ids.empty() || std::find(robot_ids.begin(), robot_ids.end(), robot_id) != robot_ids.end();
}

RobotStatusHub::RobotStatusHub(asio::io_context& io_context) : io_context_(io_context) {}

void RobotStatusHub::publish(const fms::v1::RobotStatus& status) {
    last_status_[status.robot_id()] = status;

    dv1::RobotStatusUpdate update;
    *update.mutable_status() = status;
    send_to_all(status.robot_id(), update);
}

void RobotStatusHub::remove(const std::string& robot_id) {
    if (last_status_.erase(robot_id) == 0) {
        return;
    }

    dv1::RobotStatusUpdate update;
    update.set_removed_robot_id(robot_id);
    send_to_all(robot_id, update);
}

std::shared_ptr<RobotStatusHub::Subscriber> RobotStatusHub::subscribe(std::vector<std::string> robot_ids) {
    auto subscriber = std::make_shared<Subscriber>(io_context_, std::move(robot_ids));
    dv1::RobotStatusUpdate snapshot;
    for (const auto& [robot_id, status] : last_status_) {
        if (subscriber->wants(robot_id)) {
            *snapshot.mutable_snapshot()->add_robots() = status;
        }
    }
    snapshot.mutable_snapshot();
    subscriber->channel.try_send(boost::system::error_code{}, std::move(snapshot));

    subscribers_.push_back(subscriber);
    spdlog::info("[status_hub] subscriber added, total {}", subscribers_.size());
    return subscriber;
}

void RobotStatusHub::unsubscribe(const std::shared_ptr<Subscriber>& subscriber) {
    if (std::erase(subscribers_, subscriber) > 0) {
        spdlog::info("[status_hub] subscriber removed, total {}", subscribers_.size());
    }
}

void RobotStatusHub::close_all() {
    spdlog::info("[status_hub] closing {} subscribers", subscribers_.size());
    for (auto& subscriber : subscribers_) {
        subscriber->channel.close();
    }
    subscribers_.clear();
}

void RobotStatusHub::send_to_all(const std::string& robot_id, const dv1::RobotStatusUpdate& update){
    for (auto it = subscribers_.begin(); it != subscribers_.end();) {
        auto& subscriber = **it;

        if (!subscriber.wants(robot_id)) {
            ++it;
            continue;
        }
        if (subscriber.channel.try_send(boost::system::error_code{}, update)) {
            ++it;
            continue;
        }

        spdlog::warn("[status_hub] subscriber too slow, subscription closed");
        subscriber.channel.close();
        it = subscribers_.erase(it);
    }
}