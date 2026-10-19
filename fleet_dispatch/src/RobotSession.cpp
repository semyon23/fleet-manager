#include "../include/RobotSession.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "spdlog/spdlog.h"
#include "VdaTopics.hpp"
#include "RobotStatusBuilder.hpp"
#include <google/protobuf/util/time_util.h>
using google::protobuf::util::TimeUtil;

RobotSession::RobotSession(asio::io_context& io_context, Outbox& outbox, std::string robot_id,
                           std::string manufacturer, std::string serial_number)
    : io_context_(io_context),
      outbox_(outbox),
      robot_id_(std::move(robot_id)),
      manufacturer_(std::move(manufacturer)),
      serial_number_(std::move(serial_number)),
      state_topic_(robot_topic(manufacturer_, serial_number_, "state")),
      connection_topic_(robot_topic(manufacturer_, serial_number_, "connection")),
      factsheet_topic_(robot_topic(manufacturer_, serial_number_, "factsheet")),
      inbox_(io_context, 100) {
    status_.set_robot_id(robot_id_);
    status_.set_connection(fms::v1::CONNECTION_STATE_UNKNOWN);
}

void RobotSession::start() {
    request(MqttAction::Subscribe, state_topic_);
    request(MqttAction::Subscribe, connection_topic_);
    request(MqttAction::Subscribe, factsheet_topic_);

    asio::co_spawn(io_context_, [self = shared_from_this()] { return self->run(); },
                   [id = robot_id_](std::exception_ptr error) {
                       if (!error) return;
                       try {
                           std::rethrow_exception(error);
                       } catch (const std::exception& e) {
                           spdlog::error("[{}] session failed: {}", id, e.what());
                       }
                   });
}

void RobotSession::stop() {
    request(MqttAction::Unsubscribe, state_topic_);
    request(MqttAction::Unsubscribe, connection_topic_);
    request(MqttAction::Unsubscribe, factsheet_topic_);
    inbox_.close();
}

bool RobotSession::post(VdaMessage message) {
    return inbox_.try_send(boost::system::error_code{}, std::move(message));
}

asio::awaitable<void> RobotSession::run() {
    spdlog::info("[{}] session started ({} / {})", robot_id_, manufacturer_, serial_number_);
    while (true) {
        auto [ec, message] = co_await inbox_.async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            break;
        }
        handle(message);
    }
    spdlog::info("[{}] session stopped", robot_id_);
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
            *status_.mutable_last_state_at() = TimeUtil::GetCurrentTime();
            log_status();
        } else if (message.topic == connection_topic_) {
            auto connection = json.get<vda5050::Connection>();
            status_.set_connection(to_proto(connection.connection_state));
            log_status();
        } else if (message.topic == factsheet_topic_) {
            factsheet_ = json.get<vda5050::Factsheet>();
            *status_.mutable_factsheet_received_at() = TimeUtil::GetCurrentTime();
            spdlog::info("[{}] factsheet received", robot_id_);
        }
    } catch (const std::exception& e) {
        count_invalid_message(message.topic, e.what());
    }
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
    spdlog::info("[{}] {} {} node={} battery={:.0f}% driving={} errors={}",
                     robot_id_,
                     fms::v1::ConnectionState_Name(status_.connection()),
                     fms::v1::OperatingMode_Name(status_.operating_mode()),
                     status_.last_node_id(),
                     status_.power().state_of_charge(),
                     status_.driving(),
                     status_.errors_size());
}


void RobotSession::count_invalid_message(const std::string& topic, const std::string& error){
    auto* protocol = status_.mutable_protocol();
    protocol->set_invalid_messages(protocol->invalid_messages() + 1);
    protocol->set_last_error(error);
    *protocol->mutable_last_error_at() = TimeUtil::GetCurrentTime();
    spdlog::warn("[{}] invalid message on {}: {}", robot_id_, topic, error);
}
