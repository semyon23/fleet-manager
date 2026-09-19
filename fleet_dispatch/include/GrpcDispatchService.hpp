#pragma once
#ifndef GRPCSERVERHANDLER_HPP
#define GRPCSERVERHANDLER_HPP


#include <memory>
#include <string>
#include <agrpc/asio_grpc.hpp>
#include <boost/asio/experimental/channel.hpp>
#include "include/types.hpp"
#include "FleetDispatchService.grpc.pb.h"
#include <print>
namespace asio = boost::asio;

using stream_status_channel = asio::experimental::channel<void(boost::system::error_code, RobotStatusUpdate)>;
using stream_visualisation_channel = asio::experimental::channel<void(boost::system::error_code, RobotVisualization)>;

using register_channel = asio::experimental::channel<void(boost::system::error_code, RegisterRobot)>;
using unregister_channel = asio::experimental::channel<void(boost::system::error_code, UnregisterRobot)>;

class GrpcDispatchService : public std::enable_shared_from_this<GrpcDispatchService> {
public:
    GrpcDispatchService(grpc::ServerBuilder& builder, agrpc::GrpcContext& context, const std::string& server_address);
    ~GrpcDispatchService();
    void start(grpc::ServerBuilder& builder) ;
    void stop();
    asio::awaitable<std::optional<RegisterRobot>> get_register_robot();
    asio::awaitable<std::optional<UnregisterRobot>> get_unregister_robot();

    void set_status_robot(RobotStatusUpdate& data);
    void set_visu_robot(RobotVisualization& data);


private:
    using RegisterRPC = agrpc::ServerRPC<&fms::FleetDispatchService::AsyncService::RequestRegisterRobot>;
    using UnregisterRPC = agrpc::ServerRPC<&fms::FleetDispatchService::AsyncService::RequestUnregisterRobot>;

    using StatusesStreamRPC = agrpc::ServerRPC<&fms::FleetDispatchService::AsyncService::RequestStreamRobotStatuses>;
    using VisualizationsStreamRPC = agrpc::ServerRPC<&fms::FleetDispatchService::AsyncService::RequestStreamRobotVisualizations>;


    asio::awaitable<void> process_register(RegisterRPC& rpc, const fms::RegisterRobotRequest& request);
    asio::awaitable<void> process_unregister(UnregisterRPC& rpc, const fms::UnregisterRobotRequest& request);

    asio::awaitable<void> process_stream_statuses(StatusesStreamRPC& rpc, const fms::StreamStatusesRequest& request);
    asio::awaitable<void> process_stream_visualizations(VisualizationsStreamRPC& rpc, const fms::StreamStatusesRequest& request);

    std::string address;
    agrpc::GrpcContext& grpc_context;
    std::unique_ptr<grpc::Server> server;
    fms::FleetDispatchService::AsyncService service;

    register_channel register_robot;
    unregister_channel unregister_robot;
    stream_status_channel status_robot;
    stream_visualisation_channel visu_robot;
};


#endif //GRPCSERVERHANDLER_HPP
