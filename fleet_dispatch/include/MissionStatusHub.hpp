
#ifndef FLEETMANAGERSYSTEM_MISSIONSTATUSHUB_HPP
#define FLEETMANAGERSYSTEM_MISSIONSTATUSHUB_HPP
#pragma once

#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fleet_dispatch.pb.h"
#include "fms_types.pb.h"

namespace asio = boost::asio;
namespace dv1 = fms::dispatch::v1;

class MissionStatusHub {
public:
    using Channel = asio::experimental::channel<void(boost::system::error_code, dv1::MissionStatusUpdate)>;

    struct Subscriber {
        Subscriber(asio::io_context& io_context, std::vector<std::string> robot_ids);

        bool wants(const std::string& robot_id) const;

        std::vector<std::string> robot_ids;   // пусто — миссии всех роботов
        Channel channel;
    };

    explicit MissionStatusHub(asio::io_context& io_context);

    void publish(const fms::v1::MissionStatus& status);   // миссия изменилась

    std::shared_ptr<Subscriber> subscribe(std::vector<std::string> robot_ids);   // первое сообщение — снимок
    void unsubscribe(const std::shared_ptr<Subscriber>& subscriber);
    void close_all();

private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        fms::v1::MissionStatus status;
        std::optional<Clock::time_point> finished_at;   // когда миссия стала COMPLETED / FAILED / CANCELED
    };

    void forget_old_finished();
    void send_to_all(const std::string& robot_id, const dv1::MissionStatusUpdate& update);

    asio::io_context& io_context_;
    std::unordered_map<std::string, Entry> missions_;
    std::vector<std::shared_ptr<Subscriber>> subscribers_;
};
#endif //FLEETMANAGERSYSTEM_MISSIONSTATUSHUB_HPP
