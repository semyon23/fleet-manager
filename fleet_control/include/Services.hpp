
#ifndef FLEETMANAGERSYSTEM_SERVICES_HPP
#define FLEETMANAGERSYSTEM_SERVICES_HPP

#pragma once

#include <agrpc/asio_grpc.hpp>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

#include "fleet_database.grpc.pb.h"
#include "fleet_dispatch.grpc.pb.h"
#include "health.grpc.pb.h"

struct Services {
    Services(agrpc::GrpcContext& grpc_context, const std::string& database_address, const std::string& dispatch_address);

    agrpc::GrpcContext& grpc_context;
    std::unique_ptr<fms::database::v1::RobotRegistry::Stub> registry;
    std::unique_ptr<fms::dispatch::v1::FleetDispatch::Stub> dispatch;
    std::unique_ptr<grpc::health::v1::Health::Stub> database_health;
    std::unique_ptr<grpc::health::v1::Health::Stub> dispatch_health;
};

#endif //FLEETMANAGERSYSTEM_SERVICES_HPP
