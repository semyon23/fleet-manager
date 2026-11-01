#include "../include/Services.hpp"


Services::Services(agrpc::GrpcContext& grpc_context, const std::string& database_address,
                   const std::string& dispatch_address)
    : grpc_context(grpc_context) {

    auto database_channel = grpc::CreateChannel(database_address, grpc::InsecureChannelCredentials());
    auto dispatch_channel = grpc::CreateChannel(dispatch_address, grpc::InsecureChannelCredentials());

    registry = fms::database::v1::RobotRegistry::NewStub(database_channel);
    dispatch = fms::dispatch::v1::FleetDispatch::NewStub(dispatch_channel);
    database_health = grpc::health::v1::Health::NewStub(database_channel);
    dispatch_health = grpc::health::v1::Health::NewStub(dispatch_channel);
}