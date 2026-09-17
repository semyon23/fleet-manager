
#include "../../include/core/FleetDispatchClient.hpp"
#include <boost/asio/detached.hpp>

FleetDispatchClient::FleetDispatchClient(agrpc::GrpcContext& context, const std::string& server_address)
    : ctx(context){
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    stub = fms::FleetDispatchService::NewStub(channel);
}

FleetDispatchClient::~FleetDispatchClient(){

}

void FleetDispatchClient::run(){
    auto self = shared_from_this();

    boost::asio::co_spawn(
        ctx.get_executor(),
        [self]() -> boost::asio::awaitable<void> {

            co_await self->stream_robot_visualizations("client");
        },
        boost::asio::detached
    );
};

asio::awaitable<fms::RegisterRobotResponse> FleetDispatchClient::register_robot(const std::string& robot_id, const std::string& manufacturer){
    using RPC = agrpc::ClientRPC<&fms::FleetDispatchService::Stub::PrepareAsyncRegisterRobot>;

    grpc::ClientContext client_context;
    client_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));


    fms::RegisterRobotRequest request;
    request.set_robot_id(robot_id);
    request.set_manufacturer(manufacturer);

    fms::RegisterRobotResponse response;

    grpc::Status status = co_await RPC::request(
        ctx, *stub, client_context, request, response, asio::use_awaitable);

    if (!status.ok()) {
        std::cerr << "[Client Error] RegisterRobot RPC failed: " << status.error_message() << std::endl;
        response.set_success(false);
        response.set_error_message(status.error_message());
    }
    co_return response;

}

asio::awaitable<fms::UnregisterRobotResponse> FleetDispatchClient::unregister_robot(const std::string& robot_id){
    using RPC = agrpc::ClientRPC<&fms::FleetDispatchService::Stub::PrepareAsyncUnregisterRobot>;

    grpc::ClientContext client_context;
    client_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    fms::UnregisterRobotRequest request;
    request.set_robot_id(robot_id);

    fms::UnregisterRobotResponse response;

    grpc::Status status = co_await RPC::request(
        ctx, *stub, client_context, request, response, asio::use_awaitable);

    if (!status.ok()) {
        std::cerr << "[Client Error] UnregisterRobot RPC failed: " << status.error_message() << std::endl;
        response.set_success(false);
    }

    co_return response;
}

asio::awaitable<void> FleetDispatchClient::stream_robot_statuses(const std::string& client_id){
    fms::StreamStatusesRequest request;
    request.set_client_id(client_id);

    StatusesStreamRPC rpc{ctx};

    if (!co_await rpc.start(*stub, request)) {
        std::cerr << "[Client Error] Failed to start Statuses Stream\n";
        co_return;
    }

    std::cout << "[Client] Successfully connected to Statuses Stream. Listening...\n";

    fms::RobotStatusUpdate response;

    while (co_await rpc.read(response)) {
        std::cout << "[Client Status Update] Robot: " << response.robot_id()
                  << ", Status: " << response.status() << "\n";
    }

    grpc::Status status = co_await rpc.finish();
    if (!status.ok()) {
        std::cerr << "[Client Statuses Stream] Ended with error: " << status.error_message() << "\n";
    }
}

asio::awaitable<void> FleetDispatchClient::stream_robot_visualizations(const std::string& client_id){
    fms::StreamStatusesRequest request;
    request.set_client_id(client_id);

    VisualizationsStreamRPC rpc{ctx};

    if (!co_await rpc.start(*stub, request)) {
        std::cerr << "[Client Error] Failed to start Visu Stream\n";
        co_return;
    }

    std::cout << "[Client] Successfully connected to Visu Stream. Listening...\n";

    fms::RobotVisualization response;

    while (co_await rpc.read(response)) {
        std::cout << "[Client Visu] Robot : " << response.robot_id()
                  << ", Status: " << response.agv_position().map_id() << "\n";
    }

    grpc::Status status = co_await rpc.finish();
    if (!status.ok()) {
        std::cerr << "[Client Visu Stream] Ended with error: " << status.error_message() << "\n";
    }
}
