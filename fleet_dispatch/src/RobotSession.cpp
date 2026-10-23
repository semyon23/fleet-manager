#include "../include/RobotSession.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "spdlog/spdlog.h"
#include "VdaTopics.hpp"
#include "RobotStatusBuilder.hpp"
#include "OrderBuilder.hpp"
#include <google/protobuf/util/time_util.h>
using google::protobuf::util::TimeUtil;

std::string action_error(const vda5050::State& state, const vda5050::ActionState& action_state) {
    for (const auto& error : state.errors) {
        if (!error.error_references) {
            continue;
        }
        for (const auto& reference : *error.error_references) {
            if (reference.reference_key == "actionId" && reference.reference_value == action_state.action_id) {
                return error.error_description.value_or(error.error_type);
            }
        }
    }
    return action_state.action_result.value_or("no details");
}

RobotSession::RobotSession(asio::io_context& io_context, Outbox& outbox, RobotStatusHub& status_hub, MissionStatusHub& mission_hub, std::string robot_id,
                           std::string manufacturer, std::string serial_number)
    : io_context_(io_context),
      outbox_(outbox),
      status_hub_(status_hub),
      mission_hub_(mission_hub),
      robot_id_(std::move(robot_id)),
      manufacturer_(std::move(manufacturer)),
      serial_number_(std::move(serial_number)),
      state_topic_(robot_topic(manufacturer_, serial_number_, "state")),
      connection_topic_(robot_topic(manufacturer_, serial_number_, "connection")),
      factsheet_topic_(robot_topic(manufacturer_, serial_number_, "factsheet")),
      inbox_(io_context, 100),
      watchdog_timer_(io_context),
      instant_actions_header_id_(static_cast<std::uint32_t>(std::time(nullptr))),
      order_header_id_(static_cast<std::uint32_t>(std::time(nullptr))) {
    status_.set_robot_id(robot_id_);
    status_.set_connection(fms::v1::CONNECTION_STATE_UNKNOWN);
}

void RobotSession::start() {
    request(MqttAction::Subscribe, state_topic_);
    request(MqttAction::Subscribe, connection_topic_);
    request(MqttAction::Subscribe, factsheet_topic_);

    status_hub_.publish(status_);
    auto log_if_failed = [id = robot_id_](std::exception_ptr error) {
        if (!error) return;
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            spdlog::error("[{}] session coroutine failed: {}", id, e.what());
        }
    };

    asio::co_spawn(io_context_, [self = shared_from_this()] { return self->run(); }, log_if_failed);
    asio::co_spawn(io_context_, [self = shared_from_this()] { return self->watchdog(); }, log_if_failed);
}

void RobotSession::stop() {
    stopped_ = true;
    request(MqttAction::Unsubscribe, state_topic_);
    request(MqttAction::Unsubscribe, connection_topic_);
    request(MqttAction::Unsubscribe, factsheet_topic_);
    inbox_.close();
    watchdog_timer_.cancel();
}

bool RobotSession::post(VdaMessage message) {
    return inbox_.try_send(boost::system::error_code{}, std::move(message));
}

asio::awaitable<void> RobotSession::run() {
    spdlog::info("[{}] session started ({} / {})", robot_id_, manufacturer_, serial_number_);
    while (true) {
        auto [ec, message] = co_await inbox_.async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec || stopped_) {
            break;
        }
        handle(message);
    }
    spdlog::info("[{}] session stopped", robot_id_);
}

asio::awaitable<void> RobotSession::watchdog() {
    while (true) {
        watchdog_timer_.expires_after(std::chrono::seconds(5));
        co_await watchdog_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        if (stopped_) {
            break;
        }

        bool online = status_.connection() == fms::v1::CONNECTION_STATE_ONLINE;
        bool silent = std::chrono::steady_clock::now() - last_state_time_ > std::chrono::seconds(60);
        if (online && silent) {
            status_.set_connection(fms::v1::CONNECTION_STATE_STALE);
            spdlog::warn("[{}] no state for 60 s, robot is STALE", robot_id_);
            status_hub_.publish(status_);
        }
        if (mission_ && mission_->on_tick(std::chrono::steady_clock::now())) {
            on_mission_changed();
        }
    }
}


void RobotSession::handle(const VdaMessage& message) {

    if (message.payload.empty()) {
        return;
    }

    try {
        auto json = nlohmann::json::parse(message.payload);

        if (message.topic == state_topic_) {
            auto state = json.get<vda5050::State>();
            fill_status(state, status_);
            check_instant_actions(state);

            if (mission_ && mission_->on_state(state)) {
                on_mission_changed();
            }

            *status_.mutable_last_state_at() = TimeUtil::GetCurrentTime();

            last_state_time_ = std::chrono::steady_clock::now();
            if (status_.connection() == fms::v1::CONNECTION_STATE_STALE) {
                status_.set_connection(fms::v1::CONNECTION_STATE_ONLINE);
            }
            log_status();
        } else if (message.topic == connection_topic_) {
            auto connection = json.get<vda5050::Connection>();
            status_.set_connection(to_proto(connection.connection_state));
            last_state_time_ = std::chrono::steady_clock::now();
            log_status();
        } else if (message.topic == factsheet_topic_) {
            factsheet_ = json.get<vda5050::Factsheet>();
            *status_.mutable_factsheet_received_at() = TimeUtil::GetCurrentTime();
            spdlog::info("[{}] factsheet received", robot_id_);
        }
    } catch (const std::exception& e) {
        count_invalid_message(message.topic, e.what());
    }

    status_hub_.publish(status_);
}

bool RobotSession::send(const std::string& kind, std::string payload) {
    return request(MqttAction::Publish, robot_topic(manufacturer_, serial_number_, kind), std::move(payload));
}

bool RobotSession::request(MqttAction action, const std::string& topic, std::string payload) {
    bool ok = outbox_.try_send(boost::system::error_code{}, MqttRequest{action, topic, std::move(payload)});
    if (!ok) {
        spdlog::warn("[{}] mqtt queue full or closed, request for {} dropped", robot_id_, topic);
    }
    return ok;
}

void RobotSession::log_status() const{
    spdlog::info("[{}] {} {} node={} battery={:.0f}% driving={} paused={} errors={}",
                  robot_id_,
                  fms::v1::ConnectionState_Name(status_.connection()),
                  fms::v1::OperatingMode_Name(status_.operating_mode()),
                  status_.last_node_id(),
                  status_.power().state_of_charge(),
                  status_.driving(),
                  status_.paused(),
                  status_.errors_size());
}


void RobotSession::count_invalid_message(const std::string& topic, const std::string& error){
    auto* protocol = status_.mutable_protocol();
    protocol->set_invalid_messages(protocol->invalid_messages() + 1);
    protocol->set_last_error(error);
    *protocol->mutable_last_error_at() = TimeUtil::GetCurrentTime();
    spdlog::warn("[{}] invalid message on {}: {}", robot_id_, topic, error);
}

bool RobotSession::pause() {
    return !send_instant_action(vda5050::action_type::kStartPause, {}).empty();
}

bool RobotSession::resume() {
    return !send_instant_action(vda5050::action_type::kStopPause, {}).empty();
}

bool RobotSession::initialize_position(const fms::v1::Pose& pose, const std::string& last_node_id) {
    return !send_instant_action(vda5050::action_type::kInitializePosition, {
        {"x", pose.x()},
        {"y", pose.y()},
        {"theta", pose.theta()},
        {"mapId", pose.map_id()},
        {"lastNodeId", last_node_id},
    }).empty();
}

std::string RobotSession::send_instant_action(const std::string& action_type, std::vector<vda5050::ActionParameter> parameters){
    vda5050::InstantActions message;
    message.header.header_id = ++instant_actions_header_id_;
    message.header.timestamp = vda5050::make_timestamp();
    message.header.manufacturer = manufacturer_;
    message.header.serial_number = serial_number_;

    vda5050::Action action;
    action.action_type = action_type;

    action.action_id = robot_id_ + "-" + action_type + "-" + std::to_string(message.header.header_id);

    action.blocking_type = vda5050::BlockingType::None;
    if (!parameters.empty()) {
        action.action_parameters = std::move(parameters);
    }
    message.actions.push_back(action);
    if (!send("instantActions", nlohmann::json(message).dump())) {
        return {};
    }
    pending_actions_[action.action_id] = action_type;
    spdlog::info("[{}] {} sent ({})", robot_id_, action_type, action.action_id);
    return action.action_id;


}

void RobotSession::check_instant_actions(const vda5050::State& state) {
    for (const auto& action_state : state.instant_action_states) {
        auto it = pending_actions_.find(action_state.action_id);
        if (it == pending_actions_.end()) {
            continue;
        }

        if (action_state.action_status == vda5050::ActionStatus::Finished) {
            spdlog::info("[{}] {} confirmed", robot_id_, it->second);
            pending_actions_.erase(it);
        } else if (action_state.action_status == vda5050::ActionStatus::Failed) {
            spdlog::warn("[{}] {} failed: {}", robot_id_, it->second, action_error(state, action_state));
            pending_actions_.erase(it);
        }
        // WAITING / INITIALIZING / RUNNING — ещё выполняется, ждём следующий state
    }
}

// 5c: построить order шага, отправить и начать следить
bool RobotSession::start_mission(const fms::v1::Mission& mission) {
    const auto& node = mission.nodes(0);
    std::string order_id = mission.mission_id() + "-" + node.id();

    vda5050::Order order = node.has_move() ? build_move_order(order_id, node.move())
                                           : build_action_order(order_id, node.action(), status_);
    order.header.header_id = ++order_header_id_;
    order.header.timestamp = vda5050::make_timestamp();
    order.header.manufacturer = manufacturer_;
    order.header.serial_number = serial_number_;

    if (!send("order", nlohmann::json(order).dump())) {
        return false;
    }

    mission_ = std::make_unique<ActiveMission>(mission, order, std::chrono::steady_clock::now());
    mission_hub_.publish(mission_->status());
    status_.set_mission_id(mission.mission_id());
    status_hub_.publish(status_);
    spdlog::info("[{}] mission {} started: order {} sent, {} nodes", robot_id_, mission.mission_id(), order_id,
                 order.nodes.size());
    return true;
}

// 5c: статус миссии изменился
void RobotSession::on_mission_changed() {
    const auto& status = mission_->status();
    const auto& step = status.steps(0);
    spdlog::info("[{}] mission {} {}, step {} {}, node {}/{}{}", robot_id_, status.mission_id(),
                 fms::v1::MissionState_Name(status.state()), step.node_id(), fms::v1::StepState_Name(step.state()),
                 step.move().last_node_index() + 1, step.move().total_nodes(),
                 status.has_error() ? " — " + status.error().message() : "");

    mission_hub_.publish(status);

    if (mission_->done()) {
        mission_.reset();
        status_.clear_mission_id();
        status_hub_.publish(status_);
    }
}

bool RobotSession::cancel_mission() {
    std::string action_id = send_instant_action(vda5050::action_type::kCancelOrder);
    if (action_id.empty()) {
        return false;
    }
    mission_->cancel(action_id, std::chrono::steady_clock::now());
    on_mission_changed();
    return true;
}

void RobotSession::abort_mission(const std::string &message) {
    if (!mission_) {
        return;
    }
    mission_->abort(fms::v1::MISSION_ERROR_CODE_ROBOT_REMOVED, message);
    on_mission_changed();
}
