
#include "../../include/core/FleetDispatchClient.hpp"
#include <boost/asio/detached.hpp>
#include <print>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/steady_timer.hpp>

#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"

FleetDispatchClient::FleetDispatchClient(agrpc::GrpcContext& context, const std::string& server_address)
    : ctx(context){
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    stub = fms::FleetDispatchService::NewStub(channel);
}

FleetDispatchClient::~FleetDispatchClient(){
    std::cout << "[ClientDispath] Destructor called\n";
}

void FleetDispatchClient::run(){
    auto self = shared_from_this();

    boost::asio::co_spawn(
        ctx.get_executor(),
        [self]() -> asio::awaitable<void> {
            co_await self->stream_robot_visualizations("client");
        },
        asio::bind_cancellation_slot(cancel_visu_stream.slot(), boost::asio::detached)
    );

    boost::asio::co_spawn(
        ctx.get_executor(),
        [self]() -> boost::asio::awaitable<void> {
            co_await self->stream_robot_statuses("client");
        },
        asio::bind_cancellation_slot(cancel_statuses_stream.slot(), boost::asio::detached)
    );
};

void FleetDispatchClient::stop(){
    static std::atomic<bool> stopped{false};
    if (stopped.exchange(true)) return;

    cancel_statuses_stream.emit(asio::cancellation_type::terminal);
    cancel_visu_stream.emit(asio::cancellation_type::terminal);
    std::cout << "[ClientDispath] Stopping client streams...\n";
}


asio::awaitable<fms::RegisterRobotResponse> FleetDispatchClient::register_robot(const std::string& robot_id, const std::string& manufacturer){
    using RPC = agrpc::ClientRPC<&fms::FleetDispatchService::Stub::PrepareAsyncRegisterRobot>;

    grpc::ClientContext client_context;
    client_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));

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
    client_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
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

void log_info(const std::string& message) {
    std::cout << GREEN << "[INFO]" << RESET << " " << message << "\n";
}

void log_error(const std::string& message) {
    std::cerr << RED << "[ERROR] " << message << RESET << "\n";
}

// template <typename RpcType, typename ResponseType, typename MessageHandler>
// asio::awaitable<void> FleetDispatchClient::stream_implimentation(const std::string& client_id, const std::string& stream_name, MessageHandler&& handler){
//     fms::StreamStatusesRequest request;
//     while (true) {
//
//         // auto alloc = co_await asio::this_coro::cancellation_state;
//         // if (alloc.cancelled() != boost::asio::cancellation_type::none) {
//         //
//         //     std::println("Statuses {} connection loop stopped: shutdown requested", stream_name);
//         //     co_return;
//         // }
//
//         RpcType rpc{ctx};
//
//         request.set_client_id(client_id);
//
//         std::println("Attempting to connect to {} Stream...", stream_name);
//
//         bool start_success = co_await rpc.start(*stub, request, asio::use_awaitable);
//
//         if (!start_success) {
//             std::println(stderr, "Failed to start {} Stream. Server might be offline", stream_name);
//         }
//         else {
//             std::println("Successfully connected to {} Stream. Listening...", stream_name);
//             ResponseType response;
//
//             while (co_await rpc.read(response, asio::use_awaitable)) {
//                 handler(response);
//             }
//             std::println("{} Stream closed by server", stream_name);
//
//             auto status = co_await rpc.finish(asio::use_awaitable);
//             if (status.error_code() == grpc::StatusCode::ABORTED) {
//                 std::println("{} Stream finish operation aborted cleanly due to shutdown", stream_name);
//                 co_return;
//             }
//         }
//
//         std::println("Reconnecting to {} Stream ...", stream_name);
//         asio::steady_timer timer(ctx.get_executor(), std::chrono::seconds(5));
//
//         auto [ec] =  co_await timer.async_wait(asio::as_tuple(boost::asio::use_awaitable));
//
//         if (ec == asio::error::operation_aborted) {
//             std::println("{} Stream retry timer aborted cleanly due to shutdown.", stream_name);
//             co_return;
//         }
//     }
// }
//
// asio::awaitable<void> FleetDispatchClient::stream_robot_statuses(const std::string& client_id){
//     return stream_implimentation<StatusesStreamRPC, fms::RobotStatusUpdate>(
//             client_id, "Statuses Stream", [](const auto& res) {
//             std::cout << "[Client Status Update] Robot: " << res.robot_id()
//                       << ", Status: " << res.status() << "\n";
//             });
// }
//
// asio::awaitable<void> FleetDispatchClient::stream_robot_visualizations(const std::string& client_id){
//     return stream_implimentation<VisualizationsStreamRPC, fms::RobotVisualization>(
//            client_id, "Visu Stream", [](const auto& res) {
//                std::cout << "[Client Visu] Robot : " << res.robot_id()
//                          << ", Status: " << res.agv_position().map_id() << "\n";
//            });
// }


asio::awaitable<void> FleetDispatchClient::stream_robot_statuses(const std::string& client_id) {
    fms::StreamStatusesRequest request;
    request.set_client_id(client_id);

    while (true) {
        auto alloc = co_await asio::this_coro::cancellation_state;
        if (alloc.cancelled() != boost::asio::cancellation_type::none) {
            log_info("Statuses Stream connection loop stopped: shutdown requested.");
            co_return;
        }

        log_info("Attempting to connect to Statuses Stream...");

        StatusesStreamRPC rpc{ctx};

        bool start_success = co_await rpc.start(*stub, request, asio::use_awaitable);

        if (!start_success) {
            log_error("Failed to start Statuses Stream. Server might be offline.");
        }
        else {
            log_info("Successfully connected to Statuses Stream. Listening...");
            fms::RobotStatusUpdate response;

            while (co_await rpc.read(response, asio::use_awaitable)) {
                std::cout << "[Client Status Update] Robot: " << response.robot_id()
                          << ", Status: " << response.status() << "\n";
            }
            log_info("Statuses Stream closed by server.");

            auto status = co_await rpc.finish(asio::use_awaitable);
            if (status.error_code() == grpc::StatusCode::ABORTED) {
                log_info("Statuses Stream finish operation aborted cleanly due to shutdown.");
                co_return;
            }
        }

        log_info("Reconnecting to Statuses Stream ...");
        asio::steady_timer timer(ctx.get_executor(), std::chrono::seconds(5));

        auto [ec] =  co_await timer.async_wait(asio::as_tuple(boost::asio::use_awaitable));

        if (ec == asio::error::operation_aborted) {
            log_info("Statuses Stream retry timer aborted cleanly due to shutdown.");
            co_return;
        }

    }
}

asio::awaitable<void> FleetDispatchClient::stream_robot_visualizations(const std::string& client_id){
    fms::StreamStatusesRequest request;
    request.set_client_id(client_id);

    while (true)
    {
        auto alloc = co_await asio::this_coro::cancellation_state;
        if (alloc.cancelled() != boost::asio::cancellation_type::none) {
            log_info("Visu Stream connection loop stopped: shutdown requested.");
            co_return;
        }

        log_info("Attempting to connect to Visu Stream...");

        VisualizationsStreamRPC rpc{ctx};

        bool start_success = co_await rpc.start(*stub, request, asio::use_awaitable);

        if (!start_success) {
            log_error("Failed to start Visu Stream. Server might be offline.");
        }else{

            log_info("Successfully connected to Visu Stream. Listening...");
            fms::RobotVisualization response;

            while (co_await rpc.read(response, asio::use_awaitable)) {
                std::cout << "[Client Visu] Robot : " << response.robot_id()
                 << ", Status: " << response.agv_position().map_id() << "\n";
            }
            log_info("Visu Stream closed by server.");

            auto status = co_await rpc.finish(asio::use_awaitable);
            if (status.error_code() == grpc::StatusCode::ABORTED) {
                log_info("Statuses Stream finish operation aborted cleanly due to shutdown.");
                co_return;
            }
        }

        log_info("Reconnecting to Visu Stream ...");
        asio::steady_timer timer(ctx.get_executor(), std::chrono::seconds(5));

        auto [ec] =  co_await timer.async_wait(asio::as_tuple(boost::asio::use_awaitable));

        if (ec == asio::error::operation_aborted) {
            log_info("Visu Stream retry timer aborted cleanly due to shutdown.");
            co_return;
        }
    }
}
