
#include "../include/DispatchService.hpp"
#include "spdlog/spdlog.h"
using AsyncService = dv1::FleetDispatch::AsyncService;

using AddRobotRPC           = agrpc::ServerRPC<&AsyncService::RequestAddRobot>;
using RemoveRobotRPC        = agrpc::ServerRPC<&AsyncService::RequestRemoveRobot>;
using AddMissionRPC         = agrpc::ServerRPC<&AsyncService::RequestAddMission>;
using CancelMissionRPC      = agrpc::ServerRPC<&AsyncService::RequestCancelMission>;
using ExtendMissionRPC      = agrpc::ServerRPC<&AsyncService::RequestExtendMission>;
using PauseRobotRPC         = agrpc::ServerRPC<&AsyncService::RequestPauseRobot>;
using ResumeRobotRPC        = agrpc::ServerRPC<&AsyncService::RequestResumeRobot>;
using InitializePositionRPC = agrpc::ServerRPC<&AsyncService::RequestInitializePosition>;
using StreamRobotStatusRPC   = agrpc::ServerRPC<&AsyncService::RequestStreamRobotStatus>;
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

DispatchService::DispatchService(agrpc::GrpcContext& grpc_context, FleetRegistry& registry)
    : grpc_context_(grpc_context), fleet_registry_(registry){}

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
        [](StreamRobotStatusRPC& rpc, StreamRobotStatusRPC::Request&) -> asio::awaitable<void> {
            co_await rpc.finish(unimplemented("StreamRobotStatus"));
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

asio::awaitable<grpc::Status> DispatchService::pause_robot(const dv1::PauseRobotRequest&, dv1::PauseRobotResponse&) {
    co_return unimplemented("PauseRobot");
}

asio::awaitable<grpc::Status> DispatchService::resume_robot(const dv1::ResumeRobotRequest&, dv1::ResumeRobotResponse&) {
    co_return unimplemented("ResumeRobot");
}

asio::awaitable<grpc::Status> DispatchService::initialize_position(const dv1::InitializePositionRequest&,
                                                                   dv1::InitializePositionResponse&) {
    co_return unimplemented("InitializePosition");
}