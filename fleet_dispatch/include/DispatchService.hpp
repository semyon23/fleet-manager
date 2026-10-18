//
// Created by ubuntu on 10/17/26.
//

#ifndef DISPATCHSERVICE_HPP
#define DISPATCHSERVICE_HPP
#pragma once

#include <agrpc/asio_grpc.hpp>
#include <boost/asio/awaitable.hpp>
#include "FleetRegistry.hpp"
#include "fleet_dispatch.grpc.pb.h"

namespace asio = boost::asio;
namespace dv1 = fms::dispatch::v1;

class DispatchService {
public:
    explicit DispatchService(agrpc::GrpcContext& grpc_context, FleetRegistry& registry);
    grpc::Service& grpc_service();
    void start();

private:
    asio::awaitable<grpc::Status> add_robot(const dv1::AddRobotRequest& request, dv1::AddRobotResponse& response);
    asio::awaitable<grpc::Status> remove_robot(const dv1::RemoveRobotRequest& request, dv1::RemoveRobotResponse& response);
    asio::awaitable<grpc::Status> add_mission(const dv1::AddMissionRequest& request, dv1::AddMissionResponse& response);
    asio::awaitable<grpc::Status> cancel_mission(const dv1::CancelMissionRequest& request, dv1::CancelMissionResponse& response);
    asio::awaitable<grpc::Status> extend_mission(const dv1::ExtendMissionRequest& request, dv1::ExtendMissionResponse& response);
    asio::awaitable<grpc::Status> pause_robot(const dv1::PauseRobotRequest& request, dv1::PauseRobotResponse& response);
    asio::awaitable<grpc::Status> resume_robot(const dv1::ResumeRobotRequest& request, dv1::ResumeRobotResponse& response);
    asio::awaitable<grpc::Status> initialize_position(const dv1::InitializePositionRequest& request, dv1::InitializePositionResponse& response);

    agrpc::GrpcContext& grpc_context_;
    FleetRegistry& fleet_registry_;
    dv1::FleetDispatch::AsyncService service_;
};



#endif //DISPATCHSERVICE_HPP
