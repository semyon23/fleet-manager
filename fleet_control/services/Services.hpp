
#ifndef FLEETMANAGERSYSTEM_SERVICES_HPP
#define FLEETMANAGERSYSTEM_SERVICES_HPP

#pragma once

#include <agrpc/asio_grpc.hpp>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <boost/asio/use_awaitable.hpp>
#include "fleet_database.grpc.pb.h"
#include "fleet_dispatch.grpc.pb.h"
#include "health.grpc.pb.h"

namespace asio = boost::asio;

struct Services {
    Services(agrpc::GrpcContext& grpc_context, const std::string& database_address, const std::string& dispatch_address);

    agrpc::GrpcContext& grpc_context;
    std::unique_ptr<fms::database::v1::RobotRegistry::Stub> registry;
    std::unique_ptr<fms::dispatch::v1::FleetDispatch::Stub> dispatch;
    std::unique_ptr<grpc::health::v1::Health::Stub> database_health;
    std::unique_ptr<grpc::health::v1::Health::Stub> dispatch_health;


    // Unary-вызов с тайм-аутом 3 с:
    //   co_await services.call<&RobotRegistry::Stub::PrepareAsyncCreateRobot>(*services.registry, request, response)
    template <auto PrepareAsync, class Stub, class Request, class Response>
    asio::awaitable<grpc::Status> call(Stub& stub, const Request& request, Response& response) {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
        co_return co_await agrpc::ClientRPC<PrepareAsync>::request(grpc_context, stub, context, request, response,
                                                                   asio::use_awaitable);
    }
};

#endif //FLEETMANAGERSYSTEM_SERVICES_HPP
