
#include "MissionStatusHub.hpp"
#include <algorithm>
#include "spdlog/spdlog.h"

namespace {

constexpr std::size_t kChannelCapacity = 1000;
constexpr auto kKeepFinished = std::chrono::minutes(10);   // сколько держать завершённую миссию в снимке

bool is_final(fms::v1::MissionState state) {
    return state == fms::v1::MISSION_STATE_COMPLETED || state == fms::v1::MISSION_STATE_FAILED ||
           state == fms::v1::MISSION_STATE_CANCELED;
}

}

MissionStatusHub::Subscriber::Subscriber(asio::io_context& io_context, std::vector<std::string> robot_ids)
    : robot_ids(std::move(robot_ids)), channel(io_context, kChannelCapacity) {}

bool MissionStatusHub::Subscriber::wants(const std::string& robot_id) const {
    return robot_ids.empty() || std::find(robot_ids.begin(), robot_ids.end(), robot_id) != robot_ids.end();
}

MissionStatusHub::MissionStatusHub(asio::io_context& io_context) : io_context_(io_context) {}

void MissionStatusHub::publish(const fms::v1::MissionStatus& status) {
    auto& entry = missions_[status.mission_id()];
    entry.status = status;
    if (!is_final(status.state())) {
        entry.finished_at.reset();
    } else if (!entry.finished_at) {
        entry.finished_at = Clock::now();
    }
    forget_old_finished();

    dv1::MissionStatusUpdate update;
    *update.mutable_status() = status;
    send_to_all(status.robot_id(), update);
}

std::shared_ptr<MissionStatusHub::Subscriber> MissionStatusHub::subscribe(std::vector<std::string> robot_ids) {
    forget_old_finished();
    auto subscriber = std::make_shared<Subscriber>(io_context_, std::move(robot_ids));

    dv1::MissionStatusUpdate snapshot;
    snapshot.mutable_snapshot();   // пустой снимок — тоже снимок: «миссий нет»
    for (const auto& [mission_id, entry] : missions_) {
        if (subscriber->wants(entry.status.robot_id())) {
            *snapshot.mutable_snapshot()->add_missions() = entry.status;
        }
    }
    subscriber->channel.try_send(boost::system::error_code{}, std::move(snapshot));

    subscribers_.push_back(subscriber);
    spdlog::info("[mission_hub] subscriber added, total {}", subscribers_.size());
    return subscriber;
}

void MissionStatusHub::unsubscribe(const std::shared_ptr<Subscriber>& subscriber) {
    if (std::erase(subscribers_, subscriber) > 0) {
        spdlog::info("[mission_hub] subscriber removed, total {}", subscribers_.size());
    }
}

void MissionStatusHub::close_all() {
    spdlog::info("[mission_hub] closing {} subscribers", subscribers_.size());
    for (auto& subscriber : subscribers_) {
        subscriber->channel.close();
    }
    subscribers_.clear();
}

void MissionStatusHub::forget_old_finished() {
    auto now = Clock::now();
    std::erase_if(missions_, [now](const auto& item) {
        return item.second.finished_at && now - *item.second.finished_at > kKeepFinished;
    });
}

void MissionStatusHub::send_to_all(const std::string& robot_id, const dv1::MissionStatusUpdate& update) {
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
        spdlog::warn("[mission_hub] subscriber too slow, subscription closed");
        subscriber.channel.close();
        it = subscribers_.erase(it);
    }
}