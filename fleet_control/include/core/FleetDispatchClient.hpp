//
// Created by ubuntu on 9/5/26.
//

#ifndef FLEETDISPATCHCLIENT_HPP
#define FLEETDISPATCHCLIENT_HPP

#include <iostream>
#include <agrpc/asio_grpc.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>

#include <memory>
#include <string>
#include <agrpc/asio_grpc.hpp>
#include <thread>
#include <FleetDispatchService.grpc.pb.h>

namespace asio = boost::asio;

class FleetDispatchClient : public std::enable_shared_from_this<FleetDispatchClient>{
public:
    FleetDispatchClient(agrpc::GrpcContext& context, const std::string& server_address);
    ~FleetDispatchClient();
    void run();

    using StatusesStreamRPC = agrpc::ClientRPC<&fms::FleetDispatchService::Stub::PrepareAsyncStreamRobotStatuses>;
    using VisualizationsStreamRPC = agrpc::ClientRPC<&fms::FleetDispatchService::Stub::PrepareAsyncStreamRobotVisualizations>;

    asio::awaitable<fms::RegisterRobotResponse> register_robot(const std::string& robot_id, const std::string& manufacturer);
    asio::awaitable<fms::UnregisterRobotResponse> unregister_robot(const std::string& robot_id);

    asio::awaitable<void> stream_robot_statuses(const std::string& client_id);
    asio::awaitable<void> stream_robot_visualizations(const std::string& client_id);

private:
    std::unique_ptr<fms::FleetDispatchService::Stub> stub;
    agrpc::GrpcContext& ctx;
};



#endif //FLEETDISPATCHCLIENT_HPP
