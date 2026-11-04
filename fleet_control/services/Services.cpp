#include "Services.hpp"


Services::Services(agrpc::GrpcContext& grpc_context, const std::string& database_address,
                   const std::string& dispatch_address)
    : grpc_context(grpc_context) {

    grpc::ChannelArguments database_args;
    database_args.SetMaxReceiveMessageSize(32 * 1024 * 1024);   // GetMap с картинкой — мегабайты, по умолчанию 4 МБ
    auto database_channel = grpc::CreateCustomChannel(database_address, grpc::InsecureChannelCredentials(),database_args);

    auto dispatch_channel = grpc::CreateChannel(dispatch_address, grpc::InsecureChannelCredentials());

    control = fms::database::v1::ControlStorage::NewStub(database_channel);

    registry = fms::database::v1::RobotRegistry::NewStub(database_channel);
    dispatch = fms::dispatch::v1::FleetDispatch::NewStub(dispatch_channel);
    database_health = grpc::health::v1::Health::NewStub(database_channel);
    dispatch_health = grpc::health::v1::Health::NewStub(dispatch_channel);
}