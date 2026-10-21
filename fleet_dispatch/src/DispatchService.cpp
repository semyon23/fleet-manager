
#include "../include/DispatchService.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include "spdlog/spdlog.h"
#include <boost/asio/experimental/awaitable_operators.hpp>

// Для стрима нужен rpc.wait_for_done(): узнать, что клиент отключился, не дожидаясь следующей записи
struct NotifyWhenDoneTraits : agrpc::DefaultServerRPCTraits {
    static constexpr bool NOTIFY_WHEN_DONE = true;
};



using AsyncService = dv1::FleetDispatch::AsyncService;
using StreamRobotStatusRPC = agrpc::ServerRPC<&AsyncService::RequestStreamRobotStatus, NotifyWhenDoneTraits>;

using AddRobotRPC           = agrpc::ServerRPC<&AsyncService::RequestAddRobot>;
using RemoveRobotRPC        = agrpc::ServerRPC<&AsyncService::RequestRemoveRobot>;
using AddMissionRPC         = agrpc::ServerRPC<&AsyncService::RequestAddMission>;
using CancelMissionRPC      = agrpc::ServerRPC<&AsyncService::RequestCancelMission>;
using ExtendMissionRPC      = agrpc::ServerRPC<&AsyncService::RequestExtendMission>;
using PauseRobotRPC         = agrpc::ServerRPC<&AsyncService::RequestPauseRobot>;
using ResumeRobotRPC        = agrpc::ServerRPC<&AsyncService::RequestResumeRobot>;
using InitializePositionRPC = agrpc::ServerRPC<&AsyncService::RequestInitializePosition>;
//using StreamRobotStatusRPC   = agrpc::ServerRPC<&AsyncService::RequestStreamRobotStatus>;
using StreamMissionStatusRPC = agrpc::ServerRPC<&AsyncService::RequestStreamMissionStatus>;

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

DispatchService::DispatchService(agrpc::GrpcContext& grpc_context, FleetRegistry& registry,  RobotStatusHub& status_hub)
    : grpc_context_(grpc_context), fleet_registry_(registry), status_hub_(status_hub) {}

grpc::Service& DispatchService::grpc_service(){
     return service_;
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
           using namespace asio::experimental::awaitable_operators;

           std::vector<std::string> robot_ids(request.robot_ids().begin(), request.robot_ids().end());
           auto subscriber = status_hub_.subscribe(std::move(robot_ids));
           grpc::Status status = grpc::Status::OK;

           auto send_updates = [&]() -> asio::awaitable<void> {
               while (true) {
                   auto [ec, update] = co_await subscriber->channel.async_receive(asio::as_tuple(asio::use_awaitable));
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
        [](StreamMissionStatusRPC& rpc, StreamMissionStatusRPC::Request&) -> asio::awaitable<void> {
            co_await rpc.finish(unimplemented("StreamMissionStatus"));
        },
        on_handler_done);
}

asio::awaitable<grpc::Status> DispatchService::add_robot(const dv1::AddRobotRequest& request,
                                                         dv1::AddRobotResponse& response) {
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
    if (!fleet_registry_.remove(request.robot_id())) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "robot " + request.robot_id() + " not found"};
    }
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DispatchService::add_mission(const dv1::AddMissionRequest&, dv1::AddMissionResponse&) {
    co_return unimplemented("AddMission");
}

asio::awaitable<grpc::Status> DispatchService::cancel_mission(const dv1::CancelMissionRequest&, dv1::CancelMissionResponse&) {
    co_return unimplemented("CancelMission");
}

asio::awaitable<grpc::Status> DispatchService::extend_mission(const dv1::ExtendMissionRequest&, dv1::ExtendMissionResponse&) {
    co_return unimplemented("ExtendMission");
}

asio::awaitable<grpc::Status> DispatchService::pause_robot(const dv1::PauseRobotRequest& request, dv1::PauseRobotResponse&) {
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
