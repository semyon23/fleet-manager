
#ifndef ROBOTSESSION_HPP
#define ROBOTSESSION_HPP

#pragma once
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "DatabaseWriter.hpp"
#include "RobotMessages.hpp"
#include "RobotStatusHub.hpp"
#include "MissionStatusHub.hpp"
#include "ActiveMission.hpp"
#include "fms_types.pb.h"
#include "vda5050.hpp"


class RobotSession : public std::enable_shared_from_this<RobotSession> {
public:
    RobotSession(asio::io_context& io_context, Outbox& outbox, RobotStatusHub& status_hub, MissionStatusHub& mission_hub,
                 DatabaseWriter& database, std::string robot_id, std::string manufacturer, std::string serial_number);

    std::optional<std::uint32_t> unpublished_revision() const;
    using MotionGuard = std::function<bool()>;
    std::uint64_t observation_epoch() const { return observation_epoch_; }
    void start();

    asio::awaitable<void> start_restored(
    const fms::database::v1::RobotRecord& record, std::string mission);

    using Observer = std::function<bool(const vda5050::State*)>;

    void observe_recovery(Observer observer) {
        observer_ = std::move(observer);
    }

    bool request_probe(const std::string& id);

    bool attach_move(ActiveMission mission, std::uint64_t checkpoint_version);

    bool awaiting_live_state() const { return awaiting_live_state_; }

    void stop();

    bool pause();
    bool resume();
    bool initialize_position(const fms::v1::Pose& pose, const std::string& last_node_id);
    bool post(VdaMessage message);
    asio::awaitable<bool> start_mission(const fms::v1::Mission& mission,fms::v1::MissionStatus& initial_status, MotionGuard guard = {});

    asio::awaitable<bool> extend_mission(std::uint32_t revision,std::uint32_t released_nodes,fms::v1::MoveProgress& progress, MotionGuard guard = {});

    asio::awaitable<bool> cancel_mission();
    bool checkpoint_pending() const {
        return checkpoint_pending_ || cancel_preparing_;
    }

    bool saving_order() const {
        return saving_order_ || checkpoint_pending_ || cancel_preparing_;
    }
    void abort_mission(const std::string& message);
    // T1: ExtendMission — base текущего шага до released_nodes. false — update не ушёл (очередь MQTT),
    // правка не применена
    const ActiveMission* mission() const { return mission_.get(); }   // nullptr — робот свободен

    const std::string& robot_id() const { return robot_id_; }
    const std::string& manufacturer() const { return manufacturer_; }
    const std::string& serial_number() const { return serial_number_; }
    const fms::v1::RobotStatus& status() const { return status_; }

private:
    std::uint64_t observation_epoch_ = 0;
    Observer observer_;
    std::optional<std::chrono::steady_clock::time_point> probe_state_at_;
    std::optional<std::uint32_t> recovery_header_;
    void lost_probe_state();
    void invalidate_observation();
    void request_observation();
    std::string observation_token_;
    std::optional<std::uint32_t> observed_header_;
    std::chrono::steady_clock::time_point observation_deadline_{};
    void start_loops();
    bool awaiting_live_state_ = false;
    asio::awaitable<void> run();
    asio::awaitable<void> watchdog();

    void handle(const VdaMessage& message);
    void count_invalid_message(const std::string& topic, const std::string& error);
    void log_status() const;
    void publish_status();                 // DB-4: в стрим всегда, в базу — важное сразу, остальное раз в 5 с
    void save_factsheet(std::string json); // DB-4: сначала база, потом factsheet_received_at в стрим

    bool send(const std::string& kind, std::string payload);
    bool request(MqttAction action, const std::string& topic, std::string payload = {});
    bool send_order(vda5050::Order& order);   // заполнить заголовок и отправить (order и order update)
    std::string send_instant_action(const std::string& action_type, std::vector<vda5050::ActionParameter> parameters = {}, std::string id = {});
    void check_instant_actions(const vda5050::State& state);
    void on_mission_changed();

    void publish_mission_change();

    fms::database::v1::SaveMissionExecutionRequest checkpoint(const ActiveMission& source);
    asio::awaitable<bool> persist(fms::database::v1::SaveMissionExecutionRequest request);

    bool checkpoint_pending_ = false;
    bool cancel_preparing_ = false;
    std::uint64_t checkpoint_version_ = 0;
    std::string stored_wait_;
    std::string cancel_id_;
    bool stopped_after_cancel_ = false;

    asio::awaitable<bool> send_step_order(MotionGuard guard = {});
    asio::awaitable<bool> save_and_send(vda5050::Order order, const ActiveMission& proposed, MotionGuard guard);
    using Inbox = asio::experimental::channel<void(boost::system::error_code, VdaMessage)>;

    asio::io_context& io_context_;
    Outbox& outbox_;
    RobotStatusHub& status_hub_;
    MissionStatusHub& mission_hub_;
    DatabaseWriter& database_;
    std::string robot_id_;
    std::string manufacturer_;
    std::string serial_number_;
    std::string state_topic_;
    std::string connection_topic_;
    std::string factsheet_topic_;
    Inbox inbox_;
    fms::v1::RobotStatus status_;
    fms::v1::RobotStatus saved_status_;                   // DB-4: последний отданный в базу
    std::chrono::steady_clock::time_point saved_at_{};    // DB-4: когда; {} — ещё не отдавали
    std::optional<vda5050::Factsheet> factsheet_;

    asio::steady_timer watchdog_timer_;
    std::chrono::steady_clock::time_point last_state_time_;
    std::uint32_t instant_actions_header_id_;
    std::uint32_t order_header_id_;
    std::unordered_map<std::string, std::string> pending_actions_;
    std::unique_ptr<ActiveMission> mission_;
    std::optional<fms::database::v1::SaveMissionExecutionRequest> unpublished_update_;
    bool saving_order_ = false;
    bool stopped_ = false;
};
#endif //ROBOTSESSION_HPP
