
#include "../include/DispatchService.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include "spdlog/spdlog.h"
#include <boost/asio/experimental/awaitable_operators.hpp>
#include "../include/OrderBuilder.hpp"
#include <set>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

struct NotifyWhenDoneTraits : agrpc::DefaultServerRPCTraits {
    static constexpr bool NOTIFY_WHEN_DONE = true;
};

using AsyncService = dv1::FleetDispatch::AsyncService;
using StreamRobotStatusRPC = agrpc::ServerRPC<&AsyncService::RequestStreamRobotStatus, NotifyWhenDoneTraits>;
using StreamMissionStatusRPC = agrpc::ServerRPC<&AsyncService::RequestStreamMissionStatus, NotifyWhenDoneTraits>;

using AddRobotRPC           = agrpc::ServerRPC<&AsyncService::RequestAddRobot>;
using RemoveRobotRPC        = agrpc::ServerRPC<&AsyncService::RequestRemoveRobot>;
using AddMissionRPC         = agrpc::ServerRPC<&AsyncService::RequestAddMission>;
using CancelMissionRPC      = agrpc::ServerRPC<&AsyncService::RequestCancelMission>;
using ExtendMissionRPC      = agrpc::ServerRPC<&AsyncService::RequestExtendMission>;
using PauseRobotRPC         = agrpc::ServerRPC<&AsyncService::RequestPauseRobot>;
using ResumeRobotRPC        = agrpc::ServerRPC<&AsyncService::RequestResumeRobot>;
using InitializePositionRPC = agrpc::ServerRPC<&AsyncService::RequestInitializePosition>;

using GetMissionRPC         = agrpc::ServerRPC<&AsyncService::RequestGetMission>;

const auto on_handler_done = [](std::exception_ptr error) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        spdlog::error("[dispatch_service] rpc handler failed: {}", e.what());
    }
};
grpc::Status unimplemented(const char* name) {
    return {grpc::StatusCode::UNIMPLEMENTED, std::string(name) + ": not implemented yet"};
}

DispatchService::DispatchService(agrpc::GrpcContext& grpc_context, FleetRegistry& registry,  RobotStatusHub& status_hub, MissionStatusHub& mission_hub, DatabaseWriter& database)
    : database_(database), grpc_context_(grpc_context), fleet_registry_(registry), status_hub_(status_hub), mission_hub_(mission_hub) {}

grpc::Service& DispatchService::grpc_service(){
     return service_;
}

grpc::Status DispatchService::startup_gate(bool needs_clear, const std::string& motion_robot) const {
    const auto* inv = fleet_registry_.inventory();
    if (!inv) return {grpc::StatusCode::UNAVAILABLE, "dispatch inventory loading"};
    if (needs_clear && !inv->reason.empty()) return {grpc::StatusCode::UNAVAILABLE, inv->reason};
    if (!motion_robot.empty()) {
        if (inv->deleting.contains(motion_robot))
            return {grpc::StatusCode::FAILED_PRECONDITION, "robot pending deletion"};
        auto* robot = fleet_registry_.find(motion_robot);
        if (robot && robot->awaiting_live_state())
            return {grpc::StatusCode::UNAVAILABLE, "waiting for robot state"};
    }
    return grpc::Status::OK;
}

void DispatchService::start() {

    agrpc::register_awaitable_rpc_handler<AddRobotRPC>(
        grpc_context_, service_,
        [this](AddRobotRPC& rpc, AddRobotRPC::Request& request) -> asio::awaitable<void> {
            AddRobotRPC::Response response;
            grpc::Status status = co_await add_robot(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<RemoveRobotRPC>(
        grpc_context_, service_,
        [this](RemoveRobotRPC& rpc, RemoveRobotRPC::Request& request) -> asio::awaitable<void> {
            RemoveRobotRPC::Response response;
            grpc::Status status = co_await remove_robot(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<AddMissionRPC>(
        grpc_context_, service_,
        [this](AddMissionRPC& rpc, AddMissionRPC::Request& request) -> asio::awaitable<void> {
            AddMissionRPC::Response response;
            grpc::Status status = co_await add_mission(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<CancelMissionRPC>(
        grpc_context_, service_,
        [this](CancelMissionRPC& rpc, CancelMissionRPC::Request& request) -> asio::awaitable<void> {
            CancelMissionRPC::Response response;
            grpc::Status status = co_await cancel_mission(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<ExtendMissionRPC>(
        grpc_context_, service_,
        [this](ExtendMissionRPC& rpc, ExtendMissionRPC::Request& request) -> asio::awaitable<void> {
            ExtendMissionRPC::Response response;
            grpc::Status status = co_await extend_mission(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<PauseRobotRPC>(
        grpc_context_, service_,
        [this](PauseRobotRPC& rpc, PauseRobotRPC::Request& request) -> asio::awaitable<void> {
            PauseRobotRPC::Response response;
            grpc::Status status = co_await pause_robot(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<ResumeRobotRPC>(
        grpc_context_, service_,
        [this](ResumeRobotRPC& rpc, ResumeRobotRPC::Request& request) -> asio::awaitable<void> {
            ResumeRobotRPC::Response response;
            grpc::Status status = co_await resume_robot(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<InitializePositionRPC>(
        grpc_context_, service_,
        [this](InitializePositionRPC& rpc, InitializePositionRPC::Request& request) -> asio::awaitable<void> {
            InitializePositionRPC::Response response;
            grpc::Status status = co_await initialize_position(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);


    agrpc::register_awaitable_rpc_handler<StreamRobotStatusRPC>(
       grpc_context_, service_,
       [this](StreamRobotStatusRPC& rpc, StreamRobotStatusRPC::Request& request) -> asio::awaitable<void> {
           if (auto g = startup_gate(false); !g.ok()) {
                co_await rpc.finish(g);
                co_return;
            }
            using namespace asio::experimental::awaitable_operators;

           std::vector<std::string> robot_ids(request.robot_ids().begin(), request.robot_ids().end());
           auto subscriber = status_hub_.subscribe(std::move(robot_ids));
           grpc::Status status = grpc::Status::OK;

           auto send_updates = [&]() -> asio::awaitable<void> {
               while (true) {
                   auto received = co_await subscriber->channel.async_receive(asio::as_tuple(asio::use_awaitable));
                    auto& [ec, update] = received;
                   if (ec) {
                       // hub закрыл очередь: подписчик не успевал читать или dispatch останавливается
                       status = grpc::Status{grpc::StatusCode::UNAVAILABLE, "status stream closed, subscribe again"};
                       co_return;
                   }
                   if (!co_await rpc.write(update)) {
                       co_return;   // клиент отключился во время записи
                   }
               }
           };
           co_await (send_updates() || rpc.wait_for_done(asio::use_awaitable));

           status_hub_.unsubscribe(subscriber);
           co_await rpc.finish(status);
       },
       on_handler_done);

    agrpc::register_awaitable_rpc_handler<StreamMissionStatusRPC>(
        grpc_context_, service_,
        [this](StreamMissionStatusRPC& rpc, StreamMissionStatusRPC::Request& request) -> asio::awaitable<void> {
            if (auto g = startup_gate(true); !g.ok()) {
                co_await rpc.finish(g);
                co_return;
            }
            using namespace asio::experimental::awaitable_operators;

            std::vector<std::string> robot_ids(request.robot_ids().begin(), request.robot_ids().end());
            auto subscriber = mission_hub_.subscribe(std::move(robot_ids));
            grpc::Status status = grpc::Status::OK;

            auto send_updates = [&]() -> asio::awaitable<void> {
                while (true) {
                    auto received = co_await subscriber->channel.async_receive(asio::as_tuple(asio::use_awaitable));
                    auto& [ec, update] = received;
                    if (ec) {
                        status = grpc::Status{grpc::StatusCode::UNAVAILABLE, "mission stream closed, subscribe again"};
                        co_return;
                    }
                    if (!co_await rpc.write(update)) {
                        co_return;
                    }
                }
            };

            co_await (send_updates() || rpc.wait_for_done(asio::use_awaitable));

            mission_hub_.unsubscribe(subscriber);
            co_await rpc.finish(status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<GetMissionRPC>(
        grpc_context_, service_,
        [this](GetMissionRPC& rpc, GetMissionRPC::Request& request) -> asio::awaitable<void> {
            GetMissionRPC::Response response;
            grpc::Status status = co_await get_mission(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);
}

asio::awaitable<grpc::Status> DispatchService::add_robot(const dv1::AddRobotRequest& request,
                                                         dv1::AddRobotResponse& response) {
    if (auto g = startup_gate(!fleet_registry_.find(request.robot_id())); !g.ok()) co_return g;
    if (request.robot_id().empty() || request.manufacturer().empty() || request.serial_number().empty()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT,
                               "robot_id, manufacturer and serial_number are required"};
    }

    AddResult result = fleet_registry_.add(request.robot_id(), request.manufacturer(), request.serial_number());
    if (result == AddResult::Conflict) {
        co_return grpc::Status{grpc::StatusCode::ALREADY_EXISTS,
                               "robot " + request.robot_id() + " or " + request.manufacturer() + "/" +
                                   request.serial_number() + " is already registered with other data"};
    }

    *response.mutable_status() = fleet_registry_.find(request.robot_id())->status();
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DispatchService::remove_robot(const dv1::RemoveRobotRequest& request,
                                                            dv1::RemoveRobotResponse&) {
    if (auto g = startup_gate(true); !g.ok()) co_return g;
    if (!fleet_registry_.remove(request.robot_id())) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "robot " + request.robot_id() + " not found"};
    }
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DispatchService::add_mission(
    const dv1::AddMissionRequest& request, dv1::AddMissionResponse& response) {
    if (auto g = startup_gate(false); !g.ok()) co_return g;
    using Receipt = fms::database::v1::DispatchAddReceiptRequest;
    using Answer = fms::database::v1::DispatchAddReceiptResponse;
    const auto& mission = request.mission();
    if (mission.mission_id().empty() || mission.robot_id().empty()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "mission_id and robot_id are required"};
    }

    Receipt receipt;
    receipt.set_mission_id(mission.mission_id());
    receipt.set_attempt(request.attempt());
    {
        google::protobuf::io::StringOutputStream sink(receipt.mutable_request_bytes());
        google::protobuf::io::CodedOutputStream output(&sink);
        output.SetSerializationDeterministic(true);
        if (!request.SerializeToCodedStream(&output) || output.HadError()) {
            co_return grpc::Status{grpc::StatusCode::INTERNAL, "cannot serialize AddMission"};
        }
    }

    auto replay = [&](const Answer& answer) -> grpc::Status {
        if (answer.state() == Answer::HAS_REPLY &&
            response.ParseFromString(answer.reply_bytes()) && response.has_status() &&
            response.status().mission_id() == mission.mission_id() &&
            response.status().robot_id() == mission.robot_id()) {
            return grpc::Status::OK;
        }
        return grpc::Status{grpc::StatusCode::UNAVAILABLE, "dispatch receipt requires reconciliation"};
    };
    auto storage_failure = [](const grpc::Status& error) -> grpc::Status {
        if (error.error_code() == grpc::StatusCode::ALREADY_EXISTS) return error;
        return grpc::Status{grpc::StatusCode::UNAVAILABLE, "dispatch receipt: " + error.error_message()};
    };

    Answer answer;
    receipt.set_operation(Receipt::LOOKUP);
    auto status = co_await database_.dispatch_add_receipt(receipt, answer);
    if (!status.ok()) co_return storage_failure(status);
    if (answer.state() != Answer::MISSING) co_return replay(answer);

    RobotSession* session = nullptr;
    status = validate_add(mission, session);
    if (!status.ok()) co_return status;

    receipt.set_operation(Receipt::CLAIM);
    answer.Clear();
    status = co_await database_.dispatch_add_receipt(receipt, answer);
    if (!status.ok()) co_return storage_failure(status);
    if (answer.state() != Answer::CLAIMED_NOW) co_return replay(answer);

    // Пока ждали запись, сессия могла исчезнуть или робот мог сменить позицию.
    session = nullptr;
    status = validate_add(mission, session);
    if (!status.ok()) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE,
                               "robot changed after dispatch claim: " + status.error_message()};
    }
    auto owner = session->shared_from_this();
    if (!(co_await owner->start_mission(mission, *response.mutable_status()))) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE,
                               "dispatch claim exists, order was not queued"};
    }

    *response.mutable_status() = session->mission()->status();
    receipt.set_operation(Receipt::SAVE_REPLY);
    receipt.set_reply_bytes(response.SerializeAsString());
    answer.Clear();
    status = co_await database_.dispatch_add_receipt(receipt, answer);
    if (!status.ok()) co_return storage_failure(status);
    co_return replay(answer);
}

grpc::Status DispatchService::validate_add(const fms::v1::Mission& mission, RobotSession*& session) {
    if (auto g = startup_gate(true, mission.robot_id()); !g.ok()) return g;
    grpc::Status status;
    session = find_online_robot(mission.robot_id(), status);
    if (!session) {
        return status;
    }

    if (RobotSession* other = fleet_registry_.find_by_mission(mission.mission_id()); other && other != session) {
        return grpc::Status{grpc::StatusCode::ALREADY_EXISTS,
                               "mission " + mission.mission_id() + " is already running on robot " + other->robot_id()};
    }

    if (const ActiveMission* active = session->mission()) {
        if (active->mission_id() == mission.mission_id()) {
            return grpc::Status{grpc::StatusCode::UNAVAILABLE, "active mission has no confirmed receipt reply"};
        }
        return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                               "robot " + mission.robot_id() + " is busy with mission " + active->mission_id()};
    }

    if (session->saving_order()) {
        return grpc::Status{grpc::StatusCode::UNAVAILABLE, "previous order write still pending"};
    }

    if (mission.nodes().empty()) {
        return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "mission has no steps"};
    }

    // 7: все шаги по порядку; robot_at — где робот окажется перед очередным шагом
    std::string robot_at = session->status().last_node_id();
    std::set<std::string> step_ids;
    for (const auto& node : mission.nodes()) {
        const std::string step = "step '" + node.id() + "': ";
        if (node.id().empty() || !step_ids.insert(node.id()).second) {
            return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "step ids must be non-empty and unique"};
        }
        if (!node.parent_id().empty()) {
            return grpc::Status{grpc::StatusCode::UNIMPLEMENTED, step + "nested steps are not supported yet"};
        }

        if (node.has_move()) {
            if (auto error = validate_move_step(node.move()); !error.empty()) {
                return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, step + error};
            }
            const auto& start = node.move().nodes(0).node_id();
            if (start != robot_at) {
                return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                                       step + "robot will be at '" + robot_at + "', route starts at '" + start + "'"};
            }
            robot_at = node.move().nodes(node.move().nodes_size() - 1).node_id();
        } else if (node.has_action()) {
            if (auto error = validate_action_step(node.action()); !error.empty()) {
                return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, step + error};
            }
            if (robot_at.empty()) {
                return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                                       step + "robot is not on the graph, call InitializePosition first"};
            }
        } else if (node.has_wait()) {
            if (!node.wait().has_duration()) {
                return grpc::Status{grpc::StatusCode::UNIMPLEMENTED, step + "waiting for a signal is not supported yet"};
            }
        } else {
            return grpc::Status{grpc::StatusCode::UNIMPLEMENTED, step + "notify steps are not supported yet"};
        }
    }

    return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DispatchService::cancel_mission(const dv1::CancelMissionRequest& request, dv1::CancelMissionResponse&) {
    if (auto g = startup_gate(true); !g.ok()) co_return g;
    RobotSession* session = fleet_registry_.find_by_mission(request.mission_id());
    if (!session) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "mission " + request.mission_id() + " is not running"};
    }
    if (session->mission()->canceling()) {
        co_return grpc::Status::OK;   // повтор: отмена уже идёт
    }
    auto connection = session->status().connection();
    if (connection != fms::v1::CONNECTION_STATE_ONLINE) {
        co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                               "robot " + session->robot_id() + " is " + fms::v1::ConnectionState_Name(connection)};
    }
    auto owner = session->shared_from_this();
    if (!(co_await owner->cancel_mission())) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE,
                               "cancellation not ready, try again"};
    }
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DispatchService::extend_mission(const dv1::ExtendMissionRequest& request,
                                                              dv1::ExtendMissionResponse& response) {
    if (auto g = startup_gate(true); !g.ok()) co_return g;
    if (request.has_replace_tail()) {
        co_return grpc::Status{grpc::StatusCode::UNIMPLEMENTED, "replace_tail is not supported yet"};
    }
    if (request.revision() == 0) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "revision starts at 1"};
    }
    RobotSession* session = fleet_registry_.find_by_mission(request.mission_id());
    if (!session) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "mission " + request.mission_id() + " is not running"};
    }
    const ActiveMission& mission = *session->mission();
    if (mission.canceling()) {
        co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION, "mission " + request.mission_id() + " is being canceled"};
    }

    if (mission.done()) {
        co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION, "mission has ended"};
    }

    const auto& step = mission.current_step();
    if (step.id() != request.move_node_id() || !step.has_move()) {
        co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                               "step " + request.move_node_id() + " is not the current move step (current: " + step.id() + ")"};
    }

    const auto& progress = mission.status().steps(mission.current_index()).move();
    const std::uint32_t last = progress.revision();
    const std::uint32_t released = mission.released_nodes();
    const std::uint32_t wanted = request.has_released_nodes() ? request.released_nodes() : released;
    const std::string revision = "revision " + std::to_string(request.revision());

    // Повтор той же правки (после обрыва) — ничего не делаем
    if (request.revision() == last) {
        if (wanted != released) {
            co_return grpc::Status{grpc::StatusCode::ALREADY_EXISTS, revision + " was already applied with other data"};
        }
        *response.mutable_progress() = progress;
        co_return grpc::Status::OK;
    }
    if (request.revision() < last) {
        co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                               revision + " is outdated, last applied is " + std::to_string(last)};
    }
    if (request.revision() > last + 1) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT,
                               revision + " skips revision " + std::to_string(last + 1)};
    }
    const auto total = static_cast<std::uint32_t>(step.move().nodes_size());
    if (wanted < released || wanted > total) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT,
                               "released_nodes must be from " + std::to_string(released) + " (base can't be revoked) to " +
                                   std::to_string(total)};
    }
    auto connection = session->status().connection();
    if (connection != fms::v1::CONNECTION_STATE_ONLINE) {
        co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                               "robot " + session->robot_id() + " is " + fms::v1::ConnectionState_Name(connection)};
    }

    auto owner = session->shared_from_this();
    if (!(co_await owner->extend_mission(request.revision(), wanted, *response.mutable_progress()))) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE, "order update was not queued"};
    }
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DispatchService::pause_robot(const dv1::PauseRobotRequest& request, dv1::PauseRobotResponse&) {
    if (auto g = startup_gate(false); !g.ok()) co_return g;
    grpc::Status status;
    RobotSession* session = find_online_robot(request.robot_id(), status);
    if (!session) {
        co_return status;
    }
    if (!session->pause()) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE, "mqtt queue is full, try again"};
    }
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DispatchService::resume_robot(const dv1::ResumeRobotRequest& request, dv1::ResumeRobotResponse&) {
    if (auto g = startup_gate(true, request.robot_id()); !g.ok()) co_return g;
    grpc::Status status;
    RobotSession* session = find_online_robot(request.robot_id(), status);
    if (!session) {
        co_return status;
    }
    if (!session->resume()) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE, "mqtt queue is full, try again"};
    }
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DispatchService::initialize_position(const dv1::InitializePositionRequest& request, dv1::InitializePositionResponse&) {
    if (auto g = startup_gate(true, request.robot_id()); !g.ok()) co_return g;
    if (request.pose().map_id().empty() || request.last_node_id().empty()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "pose.map_id and last_node_id are required"};
    }

    grpc::Status status;
    RobotSession* session = find_online_robot(request.robot_id(), status);
    if (!session) {
        co_return status;
    }
    if (!session->initialize_position(request.pose(), request.last_node_id())) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE, "mqtt queue is full, try again"};
    }
    co_return grpc::Status::OK;
}

RobotSession* DispatchService::find_online_robot(const std::string& robot_id, grpc::Status& status){
    RobotSession* session = fleet_registry_.find(robot_id);
    if (!session) {
        status = grpc::Status{grpc::StatusCode::NOT_FOUND, "robot " + robot_id + " not found"};
        return nullptr;
    }
    auto connection = session->status().connection();
    if (connection != fms::v1::CONNECTION_STATE_ONLINE) {

        status = grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                              "robot " + robot_id + " is " + fms::v1::ConnectionState_Name(connection)};
        return nullptr;
    }
    return session;
}

asio::awaitable<grpc::Status> DispatchService::get_mission(const dv1::GetMissionRequest& request,
                                                           dv1::GetMissionResponse& response) {
    if (auto g = startup_gate(true); !g.ok()) co_return g;
    RobotSession* session = fleet_registry_.find_by_mission(request.mission_id());
    if (!session) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "mission " + request.mission_id() + " is not running"};
    }

    if (session->checkpoint_pending()) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE, "checkpoint pending"};
    }

    *response.mutable_mission() = session->mission()->mission();
    *response.mutable_status() = session->mission()->status();
    co_return grpc::Status::OK;
}