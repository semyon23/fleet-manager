#include "AvailabilityTracker.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <google/protobuf/util/message_differencer.h>
#include <spdlog/spdlog.h>

#include "fleet/Availability.hpp"

namespace dbv1 = fms::database::v1;
using Registry = dbv1::RobotRegistry::Stub;

namespace {

std::string describe(const dbv1::RobotAvailability& availability) {
    std::string text = dbv1::AvailabilityState_Name(availability.state()).substr(sizeof("AVAILABILITY_STATE_") - 1);
    for (int i = 0; i < availability.reasons_size(); ++i) {
        text += (i == 0 ? " (" : ", ") + availability.reasons(i).code();
    }
    return availability.reasons_size() > 0 ? text + ")" : text;
}

}  // namespace

AvailabilityTracker::AvailabilityTracker(asio::io_context& io_context, Services& services, double low_battery_percent)
    : io_context_(io_context),
      services_(services),
      low_battery_percent_(low_battery_percent),
      wakeup_(io_context, 1),
      retry_timer_(io_context) {}

void AvailabilityTracker::start() {
    asio::co_spawn(io_context_, write_loop(), [](std::exception_ptr error) {
        if (!error) return;
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            spdlog::error("[availability] writer failed: {}", e.what());
        }
    });
}

void AvailabilityTracker::stop() {
    stopped_ = true;
    wakeup_.close();
    retry_timer_.cancel();
}

// ==================================================================== входы

void AvailabilityTracker::on_status(const fms::v1::RobotStatus& status) {
    Robot& robot = robots_[status.robot_id()];
    robot.status = status;
    robot.status_from_stream = true;
    update(status.robot_id());
}

void AvailabilityTracker::on_robot(const dbv1::RobotRecord& record) {
    const std::string& robot_id = record.spec().robot_id();
    Robot& robot = robots_[robot_id];
    robot.deleting = record.spec().lifecycle() == dbv1::ROBOT_LIFECYCLE_PENDING_DELETE;
    if (!robot.status_from_stream && record.has_status()) {
        robot.status = record.status();
    }
    update(robot_id);
}

void AvailabilityTracker::set_deleting(const std::string& robot_id) {
    robots_[robot_id].deleting = true;
    update(robot_id);
}

void AvailabilityTracker::set_dispatch_connected(bool connected) {
    dispatch_connected_ = connected;
    for (auto& [robot_id, robot] : robots_) {
        if (!connected) {
            robot.status_from_stream = false;   // статусы устарели; после переподключения придёт свежий снимок
        }
        update(robot_id);
    }
}

void AvailabilityTracker::keep_only(const std::set<std::string>& robot_ids) {
    std::erase_if(robots_, [&](const auto& item) { return !robot_ids.contains(item.first); });
    std::erase_if(current_, [&](const auto& item) { return !robot_ids.contains(item.first); });
    std::erase_if(pending_, [&](const auto& item) { return !robot_ids.contains(item.first); });
}

// ==================================================================== расчёт и запись

void AvailabilityTracker::update(const std::string& robot_id) {
    const Robot& robot = robots_[robot_id];
    dbv1::RobotAvailability availability;
    if (robot.deleting) {
        availability.set_state(dbv1::AVAILABILITY_STATE_DELETING);
    } else if (!dispatch_connected_) {
        availability.set_state(dbv1::AVAILABILITY_STATE_OFFLINE);
        auto* reason = availability.add_reasons();
        reason->set_code("DISPATCH_UNAVAILABLE");
        reason->set_message("fms has no connection to fleet_dispatch");
    } else {
        availability = availability_of(robot.status ? &*robot.status : nullptr, low_battery_percent_);
    }

    auto it = current_.find(robot_id);
    if (it != current_.end() && google::protobuf::util::MessageDifferencer::Equals(it->second, availability)) {
        return;   // не изменилась — писать нечего
    }
    spdlog::info("[availability] {}: {}", robot_id, describe(availability));
    current_[robot_id] = availability;
    pending_[robot_id] = availability;
    wakeup_.try_send(boost::system::error_code{});   // false — сигнал уже лежит
}

asio::awaitable<void> AvailabilityTracker::write_loop() {
    while (!stopped_) {
        if (pending_.empty()) {
            auto [ec] = co_await wakeup_.async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec) {
                break;   // stop()
            }
            continue;
        }

        auto batch = std::move(pending_);
        pending_.clear();
        bool retry = false;
        for (auto& [robot_id, availability] : batch) {
            dbv1::SaveRobotAvailabilityRequest request;
            request.set_robot_id(robot_id);
            *request.mutable_availability() = availability;
            dbv1::SaveRobotAvailabilityResponse response;
            grpc::Status status =
                co_await services_.call<&Registry::PrepareAsyncSaveRobotAvailability>(*services_.registry, request, response);

            if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
                status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
                pending_.try_emplace(robot_id, std::move(availability));   // за это время могла прийти новее — она важнее
                retry = true;
            } else if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {   // NOT_FOUND: робот удалён
                spdlog::warn("[availability] {} not saved: {}", robot_id, status.error_message());
            }
        }

        if (retry && !stopped_) {
            retry_timer_.expires_after(std::chrono::seconds(2));
            co_await retry_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        }
    }
}