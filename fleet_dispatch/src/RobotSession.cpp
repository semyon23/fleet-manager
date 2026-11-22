#include "../include/RobotSession.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "spdlog/spdlog.h"
#include "../include/VdaTopics.hpp"
#include "../include/RobotStatusBuilder.hpp"
#include "../include/OrderBuilder.hpp"
#include <google/protobuf/util/message_differencer.h>
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

// Номер для actionId — общий на все сессии: id не повторится, даже если робота удалили
// и сразу добавили снова (новая сессия в ту же секунду). Старт от времени — не повторится
// и после перезапуска dispatch (до 1000 action'ов в секунду).
std::uint64_t next_action_number() {
    static std::uint64_t number = static_cast<std::uint64_t>(std::time(nullptr)) * 1000;
    return ++number;
}

// DB-4: поля, которые меняются почти в каждом state — в базу их пишем не чаще раза в 5 с
fms::v1::RobotStatus without_frequent_fields(fms::v1::RobotStatus status) {
    status.clear_pose();
    status.clear_last_state_at();
    status.mutable_power()->clear_state_of_charge();
    status.mutable_power()->clear_voltage();
    status.mutable_power()->clear_range();
    return status;
}

// DB-4: MessageDifferencer, а не сравнение SerializeAsString(): порядок map при сериализации не гарантирован
bool important_change(const fms::v1::RobotStatus& saved, const fms::v1::RobotStatus& current) {
    return !google::protobuf::util::MessageDifferencer::Equals(without_frequent_fields(saved),
                                                               without_frequent_fields(current));
}

RobotSession::RobotSession(asio::io_context& io_context, Outbox& outbox, RobotStatusHub& status_hub, MissionStatusHub& mission_hub,
                           DatabaseWriter& database, std::string robot_id, std::string manufacturer, std::string serial_number)
    : io_context_(io_context),
      outbox_(outbox),
      status_hub_(status_hub),
      mission_hub_(mission_hub),
      database_(database),
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

    send_instant_action(vda5050::action_type::kStateRequest);
    publish_status();
    start_loops();
}

asio::awaitable<void> RobotSession::start_restored(
    const fms::database::v1::RobotRecord& record, std::string mission) {
    if (record.has_status()) status_ = record.status();
    status_.set_connection(fms::v1::CONNECTION_STATE_UNKNOWN);
    status_.set_mission_id(std::move(mission));
    awaiting_live_state_ = true;
    status_hub_.publish(status_);
    start_loops();
    for (const auto* topic : {&state_topic_, &connection_topic_, &factsheet_topic_}) {
        MqttRequest sub{MqttAction::Subscribe, *topic, {}};
        auto [ec] = co_await outbox_.async_send(boost::system::error_code{}, std::move(sub),
                                                asio::as_tuple(asio::use_awaitable));
        if (stopped_) co_return;
        if (ec) throw boost::system::system_error(ec);
    }
}

void RobotSession::start_loops() {
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


bool RobotSession::request_probe(const std::string& id) {
    if (send_instant_action(vda5050::action_type::kStateRequest, {}, id).empty()) {
        return false;
    }

    // Срок жизни запроса восстановления отслеживает ProbeQueue.
    pending_actions_.erase(id);
    return true;
}

void RobotSession::lost_probe_state() {
    if (observer_) {
        awaiting_live_state_ = true;
        probe_state_at_.reset();
        observer_(nullptr);
    }
}

void RobotSession::stop() {
    observer_ = {};
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
        auto received = co_await inbox_.async_receive(asio::as_tuple(asio::use_awaitable));
        auto& [ec, message] = received;
        if (ec || stopped_) {
            break;
        }
        handle(message);
    }
    spdlog::info("[{}] session stopped", robot_id_);
}

asio::awaitable<void> RobotSession::watchdog() {
    while (true) {
        watchdog_timer_.expires_after(std::chrono::seconds(1));
        co_await watchdog_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        if (stopped_) {
            break;
        }

        bool online = status_.connection() == fms::v1::CONNECTION_STATE_ONLINE;
        bool silent = std::chrono::steady_clock::now() - last_state_time_ > std::chrono::seconds(60);

        if (probe_state_at_ &&
            std::chrono::steady_clock::now() - *probe_state_at_ >= std::chrono::seconds(60)) {
            lost_probe_state();
        }

        if (online && silent) {
            status_.set_connection(fms::v1::CONNECTION_STATE_STALE);
            spdlog::warn("[{}] no state for 60 s, robot is STALE", robot_id_);
            publish_status();   // DB-4
        }

        if (!checkpoint_pending_ && !cancel_preparing_ && mission_ &&
            mission_->on_tick(std::chrono::steady_clock::now())) {
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

            if (awaiting_live_state_ && !observer_) return;
            if (observer_) {
                if (!observer_(&state)) return;

                if (status_.mission_id().empty()) {
                    observer_ = {};
                } else {
                    probe_state_at_ = std::chrono::steady_clock::now();
                }
            }

            fill_status(state, status_);
            awaiting_live_state_ = false;
            check_instant_actions(state);

            if (!checkpoint_pending_ && !cancel_preparing_ && mission_ &&
                mission_->on_state(state)) {
                if (mission_->status().state() == fms::v1::MISSION_STATE_CANCELED &&
                    !state.driving) {
                    for (const auto &action: state.instant_action_states) {
                        if (action.action_id == cancel_id_ &&
                            action.action_status == vda5050::ActionStatus::Finished) {
                            stopped_after_cancel_ = true;
                        }
                    }
                }
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
            const auto next = to_proto(connection.connection_state);
            if (next != fms::v1::CONNECTION_STATE_ONLINE) lost_probe_state();
            status_.set_connection(next);
            last_state_time_ = std::chrono::steady_clock::now();
            log_status();
        } else if (message.topic == factsheet_topic_) {
            factsheet_ = json.get<vda5050::Factsheet>();
            spdlog::info("[{}] factsheet received", robot_id_);
            save_factsheet(message.payload);   // DB-4: factsheet_received_at — после записи в базу
        }
    } catch (const std::exception& e) {
        count_invalid_message(message.topic, e.what());
    }

    publish_status();   // DB-4
}

// DB-4: в стрим — всегда; в базу — сразу, если изменилось важное, иначе не чаще раза в 5 с
void RobotSession::publish_status() {
    status_hub_.publish(status_);
    if (awaiting_live_state_) return;

    auto now = std::chrono::steady_clock::now();
    if (important_change(saved_status_, status_) || now - saved_at_ >= std::chrono::seconds(5)) {
        database_.save_status(status_);
        saved_status_ = status_;
        saved_at_ = now;
    }
}

// DB-4: control, увидев новое factsheet_received_at, читает factsheet из базы — он уже должен быть там
void RobotSession::save_factsheet(std::string json) {
    auto received_at = TimeUtil::MillisecondsToTimestamp(TimeUtil::TimestampToMilliseconds(TimeUtil::GetCurrentTime()));
    asio::co_spawn(
        io_context_,
        [self = shared_from_this(), json = std::move(json), received_at]() -> asio::awaitable<void> {
            if (!co_await self->database_.save_factsheet(self->robot_id_, json, received_at)) {
                co_return;   // не записан — время в стрим не идёт (причина в логе)
            }
            if (self->stopped_) {
                co_return;   // робота удалили, пока шла запись
            }
            *self->status_.mutable_factsheet_received_at() = received_at;
            self->publish_status();
        },
        asio::detached);
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

std::string RobotSession::send_instant_action(const std::string& action_type,std::vector<vda5050::ActionParameter> parameters, std::string id) {
    vda5050::InstantActions message;

    message.header.header_id = ++instant_actions_header_id_;
    message.header.timestamp = vda5050::make_timestamp();
    message.header.manufacturer = manufacturer_;
    message.header.serial_number = serial_number_;

    vda5050::Action action;
    action.action_type = action_type;
    action.action_id = id.empty()
     ? robot_id_ + "-" + action_type + "-" + std::to_string(next_action_number())
     : std::move(id);
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

fms::database::v1::SaveMissionExecutionRequest RobotSession::checkpoint(
    const ActiveMission& source) {
    fms::database::v1::SaveMissionExecutionRequest request;
    auto* e = request.mutable_execution();
    *e->mutable_mission() = source.mission();
    *e->mutable_status() = source.status();
    e->set_checkpoint_version(++checkpoint_version_);
    e->set_cancel_action_id(cancel_id_);
    e->set_stopped_after_cancel(stopped_after_cancel_);
    return request;
}

asio::awaitable<bool> RobotSession::persist(
    fms::database::v1::SaveMissionExecutionRequest request) {
    asio::steady_timer retry(io_context_);
    for (;;) {
        auto result = co_await database_.save_execution(request);
        if (result.ok()) co_return true;

        if (result.error_code() != grpc::StatusCode::UNAVAILABLE &&
            result.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED) {
            spdlog::error("[{}] checkpoint {} rejected: {}", robot_id_,
                          request.execution().checkpoint_version(), result.error_message());
            co_return false;
            }

        retry.expires_after(std::chrono::seconds(1));
        co_await retry.async_wait(asio::use_awaitable);
    }
}

asio::awaitable<bool> RobotSession::start_mission(
    const fms::v1::Mission& mission, fms::v1::MissionStatus& initial_status) {
    checkpoint_version_ = 0;
    cancel_id_.clear();
    stopped_after_cancel_ = false;
    stored_wait_.clear();
    mission_ = std::make_unique<ActiveMission>(mission, std::chrono::steady_clock::now());

    bool sent;
    if (mission_->order_needed()) {
        saving_order_ = true;
        sent = co_await send_step_order();
        saving_order_ = false;
    } else {
        stored_wait_ = mission_->current_step().id();
        checkpoint_pending_ = true;
        sent = co_await persist(checkpoint(*mission_));
        checkpoint_pending_ = false;
        if (stopped_ || !mission_ || mission_->done()) {
            if (mission_ && mission_->done()) on_mission_changed();
            co_return false;
        }
    }

    if (!sent) {
        if (mission_ && mission_->mission_id() == mission.mission_id() &&
            !mission_->canceling() && !cancel_preparing_ && !mission_->done()) {
            mission_->abort(fms::v1::MISSION_ERROR_CODE_INTERNAL, "order not published");
            on_mission_changed();
            }
        co_return false;
    }

    initial_status = mission_->status();
    status_.set_mission_id(mission.mission_id());
    publish_status();
    mission_hub_.publish(mission_->status());
    spdlog::info("[{}] mission {} started, {} steps", robot_id_,
                 mission.mission_id(), mission.nodes_size());
    co_return true;
}

void RobotSession::on_mission_changed() {
    if (checkpoint_pending_ || cancel_preparing_) return;

    if (mission_->done() || (mission_->current_step().has_wait() &&
                             stored_wait_ != mission_->current_step().id())) {
        checkpoint_pending_ = true;
        auto record = checkpoint(*mission_);
        const bool final = mission_->done();

        if (final && mission_->status().error().code() == fms::v1::MISSION_ERROR_CODE_TIMEOUT) {
            send_instant_action(vda5050::action_type::kCancelOrder);
        }
        if (!final) stored_wait_ = mission_->current_step().id();

        asio::co_spawn(io_context_,
            [self = shared_from_this(), record = std::move(record), final]()
                -> asio::awaitable<void> {
                if (!(co_await self->persist(record))) co_return;
                self->checkpoint_pending_ = false;
                if (final) self->publish_mission_change();
                else self->on_mission_changed();
            }, asio::detached);
        return;
    }

    if (mission_->order_needed()) {
        if (saving_order_) return;
        saving_order_ = true;

        asio::co_spawn(io_context_, [self = shared_from_this()]() -> asio::awaitable<void> {
            const bool sent = co_await self->send_step_order();
            self->saving_order_ = false;
            if (!self->mission_ || self->stopped_) co_return;
            if (!sent && !self->cancel_preparing_ && self->mission_->order_needed()) {
                self->mission_->abort(fms::v1::MISSION_ERROR_CODE_INTERNAL, "order not published");
            }
            self->on_mission_changed();
        }, asio::detached);
        return;
    }

    publish_mission_change();
}

void RobotSession::publish_mission_change() {
    const auto& status = mission_->status();
    const auto& step = status.steps(mission_->current_index());

    std::string progress;
    if (step.has_move()) {
        progress = ", node " + std::to_string(step.move().last_node_index() + 1) + "/" +
                   std::to_string(step.move().total_nodes());
    }
    spdlog::info("[{}] mission {} {}, step {} {}{}{}", robot_id_, status.mission_id(),
                 fms::v1::MissionState_Name(status.state()), step.node_id(),
                 fms::v1::StepState_Name(step.state()), progress,
                 status.has_error() ? " — " + status.error().message() : "");

    mission_hub_.publish(status);
    if (mission_->done()) {
        mission_.reset();
        status_.clear_mission_id();
        if (!stopped_) publish_status();
    }
}

asio::awaitable<bool> RobotSession::cancel_mission() {
    if (cancel_preparing_ || checkpoint_pending_ || mission_->done()) co_return false;

    cancel_preparing_ = true;
    cancel_id_ = robot_id_ + "-cancelOrder-" + std::to_string(next_action_number());

    auto proposed = *mission_;
    proposed.cancel(cancel_id_, std::chrono::steady_clock::now());
    if (!(co_await persist(checkpoint(proposed)))) co_return false;

    asio::steady_timer retry(io_context_);
    while (!stopped_ && mission_ && !mission_->done()) {
        if (!send_instant_action(vda5050::action_type::kCancelOrder, {}, cancel_id_).empty()) {
            mission_->cancel(cancel_id_, std::chrono::steady_clock::now());
            cancel_preparing_ = false;
            on_mission_changed();
            co_return true;
        }
        retry.expires_after(std::chrono::seconds(1));
        co_await retry.async_wait(asio::use_awaitable);
    }

    cancel_preparing_ = false;
    if (mission_ && mission_->done()) on_mission_changed();
    co_return false;
}

void RobotSession::abort_mission(const std::string &message) {
    if (!mission_) {
        return;
    }

    send_instant_action(vda5050::action_type::kCancelOrder);
    mission_->abort(fms::v1::MISSION_ERROR_CODE_ROBOT_REMOVED, message);
    on_mission_changed();
}

asio::awaitable<bool> RobotSession::send_step_order() {
    if (cancel_preparing_ || !mission_ || !mission_->order_needed()) co_return false;

    const auto& node = mission_->current_step();
    const auto id = mission_->current_order_id();
    auto order = node.has_move() ? build_move_order(id, node.move())
                                 : build_action_order(id, node.action(), status_);

    auto proposed = *mission_;
    proposed.order_sent(order, std::chrono::steady_clock::now());
    if (!(co_await save_and_send(order, proposed))) co_return false;

    mission_->order_sent(order, std::chrono::steady_clock::now());
    spdlog::info("[{}] mission {}: step {} order {} queued", robot_id_, mission_->mission_id(),
                 mission_->current_step().id(), id);
    co_return true;
}

asio::awaitable<bool> RobotSession::save_and_send(vda5050::Order order, const ActiveMission& proposed) {
    order.header.header_id = ++order_header_id_;
    order.header.timestamp = vda5050::make_timestamp();
    order.header.manufacturer = manufacturer_;
    order.header.serial_number = serial_number_;
    const auto payload = nlohmann::json(order).dump();

    auto request = checkpoint(proposed);
    auto* execution = request.mutable_execution();

    *execution->mutable_mission() = proposed.mission();
    *execution->mutable_status() = proposed.status();
    auto* saved = execution->mutable_order();
    saved->set_order_id(order.order_id);
    saved->set_order_update_id(order.order_update_id);
    saved->set_mission_node_id(proposed.current_step().id());
    saved->set_sent_order_json(payload);

    const auto id = proposed.mission_id();
    const auto valid = [&] {
        return !stopped_ && !cancel_preparing_ && mission_ && mission_->mission_id() == id &&
               !mission_->done() && !mission_->canceling() &&
               mission_->current_order_id() == order.order_id &&
               (order.order_update_id != 0 || order.nodes.front().node_id == status_.last_node_id()) &&
               status_.connection() == fms::v1::CONNECTION_STATE_ONLINE;
    };

    try {
        asio::steady_timer retry(io_context_);
        while (valid()) {
            auto result = co_await database_.save_execution(request);
            if (!valid()) co_return false;

            if (result.ok()) {
                while (valid()) {
                    if (send("order", payload)) co_return true;
                    retry.expires_after(std::chrono::seconds(1));
                    co_await retry.async_wait(asio::use_awaitable);
                }
                co_return false;
            }

            if (result.error_code() != grpc::StatusCode::UNAVAILABLE &&
                result.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED) {
                spdlog::error("[{}] execution snapshot rejected: {}", robot_id_, result.error_message());
                co_return false;
            }

            retry.expires_after(std::chrono::seconds(1));
            co_await retry.async_wait(asio::as_tuple(asio::use_awaitable));
        }
    } catch (const std::exception& error) {
        spdlog::error("[{}] execution write for {} failed: {}", robot_id_, order.order_id, error.what());
    }
    co_return false;
}

// Order шага уже у робота — order update; ещё нет — новая base уйдёт сразу в order.
// Сначала отправка, потом правка: не ушло — control повторит ту же правку, и она не потеряется
asio::awaitable<bool> RobotSession::extend_mission(std::uint32_t revision, std::uint32_t released_nodes, fms::v1::MoveProgress& progress) {
    if (saving_order() || mission_->done() || !mission_->tracker()) co_return false;

    const OrderTracker* tracker = mission_->tracker();
    const std::uint32_t old_released = mission_->released_nodes();
    if (released_nodes > old_released) {
        fms::v1::MoveStep move = mission_->current_step().move();
        move.set_released_nodes(released_nodes);
        auto update = build_move_update(tracker->order_id(), tracker->sent_update_id() + 1,
                                        move, static_cast<int>(old_released));

        auto proposed = *mission_;
        proposed.extend(revision, released_nodes);
        saving_order_ = true;
        const bool sent = co_await save_and_send(update, proposed);
        saving_order_ = false;
        if (!sent) {
            if (mission_ && mission_->order_needed()) on_mission_changed();
            co_return false;
        }

        mission_->extend(revision, released_nodes);
        mission_->update_sent(update);
        spdlog::info("[{}] mission {}: step {} order {} update {} sent, released {}/{} nodes",
                     robot_id_, mission_->mission_id(), mission_->current_step().id(),
                     update.order_id, update.order_update_id, released_nodes, move.nodes_size());
    } else {
        auto proposed = *mission_;
        proposed.extend(revision, released_nodes);

        checkpoint_pending_ = true;
        const bool ok = co_await persist(checkpoint(proposed));
        checkpoint_pending_ = false;

        if (mission_->done()) {
            on_mission_changed();
            co_return false;
        }
        if (!ok) co_return false;
        mission_->extend(revision, released_nodes);
    }
    progress = mission_->status().steps(mission_->current_index()).move();
    mission_hub_.publish(mission_->status());
    co_return true;
}