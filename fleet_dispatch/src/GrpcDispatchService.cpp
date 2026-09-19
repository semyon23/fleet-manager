
#include "../include/GrpcDispatchService.hpp"
#include <grpcpp/server_builder.h>
#include <grpcpp/server.h>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/as_tuple.hpp>
constexpr auto use_nothrow_awaitable = boost::asio::as_tuple(asio::deferred);

GrpcDispatchService::GrpcDispatchService(grpc::ServerBuilder& builder, agrpc::GrpcContext& context, const std::string& server_address)
    : grpc_context(context),
        register_robot(grpc_context.get_executor(), 10),
        unregister_robot(grpc_context.get_executor(), 10),
        status_robot(grpc_context.get_executor(), 10),
        visu_robot(grpc_context.get_executor(), 50)
{
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
}
GrpcDispatchService::~GrpcDispatchService(){
    std::cout << "[GrpcServer] Destructor called\n";
}

void GrpcDispatchService::stop(){
    static std::atomic<bool> stopped{false};
    if (stopped.exchange(true)) return;
    std::cout << "[GrpcServer] Initiating shutdown...\n";

    register_robot.close();
    unregister_robot.close();
    status_robot.close();
    visu_robot.close();

    // 2. Штатно останавливаем gRPC сервер
    if (server) {
        // Устанавливаем дедлайн для завершения активных RPC (например, 2 секунды)
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
        server->Shutdown(deadline);

        // Ждем, пока gRPC полностью очистит свои потоки
        server->Wait();
        std::cout << "[GrpcServer] Server stopped cleanly." << std::endl;
    }
    // if (server) {
    //     server->Shutdown();
    //     std::cout << "Server stopped." << std::endl;
    // }
}

void GrpcDispatchService::start(grpc::ServerBuilder& builder){
    server = builder.BuildAndStart();

    auto self = shared_from_this();

    agrpc::register_awaitable_rpc_handler<RegisterRPC>(
       grpc_context.get_executor(), service,
       [self](RegisterRPC& rpc, fms::RegisterRobotRequest& request) -> asio::awaitable<void> {
           co_await self->process_register(rpc, request);
       },
       asio::detached
   );

    agrpc::register_awaitable_rpc_handler<UnregisterRPC>(
        grpc_context.get_executor(), service,
        [self](UnregisterRPC& rpc, fms::UnregisterRobotRequest& request) -> asio::awaitable<void> {
            co_await self->process_unregister(rpc, request);
        },
        asio::detached
    );

    agrpc::register_awaitable_rpc_handler<StatusesStreamRPC>(
            grpc_context.get_executor(), service,
            [self](StatusesStreamRPC& rpc, fms::StreamStatusesRequest& request) -> asio::awaitable<void> {
                co_await self->process_stream_statuses(rpc, request);
            }, asio::detached
        );

    agrpc::register_awaitable_rpc_handler<VisualizationsStreamRPC>(
        grpc_context.get_executor(), service,
        [self](VisualizationsStreamRPC& rpc, fms::StreamStatusesRequest& request) -> asio::awaitable<void> {
            co_await self->process_stream_visualizations(rpc, request);
        }, asio::detached
    );


    std::cout << "Server listening on " << address << std::endl;

}

asio::awaitable<void> GrpcDispatchService::process_register(RegisterRPC& rpc, const fms::RegisterRobotRequest& request){
    std::cout << "[Register] Robot ID: " << request.robot_id() << " (" << request.manufacturer() << ")\n";

    fms::RegisterRobotResponse response;
    if (request.robot_id().empty()) {
        response.set_success(false);
        response.set_error_message("Robot ID cannot be empty");
    } else {
        response.set_success(true);
    }
    RegisterRobot robot;
    robot.robot_id = request.robot_id();
    robot.manufacturer = request.manufacturer();
    register_robot.try_send(boost::system::error_code{}, robot);

    co_await rpc.finish(response, grpc::Status::OK);
}

asio::awaitable<void> GrpcDispatchService::process_unregister(UnregisterRPC& rpc, const fms::UnregisterRobotRequest& request){
    std::cout << "[Unregister] Robot ID: " << request.robot_id() << "\n";

    fms::UnregisterRobotResponse response;
    response.set_success(true);

    UnregisterRobot robot;
    robot.robot_id = request.robot_id();
    unregister_robot.try_send(boost::system::error_code{}, robot);
    co_await rpc.finish(response, grpc::Status::OK);
}

asio::awaitable<void> GrpcDispatchService::process_stream_statuses(StatusesStreamRPC& rpc, const fms::StreamStatusesRequest& request) {
    std::cout << "[StreamStatuses] Client connected: " << request.client_id() << "\n";
    if (!status_robot.is_open()) {
        status_robot = stream_status_channel{grpc_context.get_executor(), 50};
    }
    try {
        while (true) {
            auto [ec, msg_status] = co_await status_robot.async_receive(use_nothrow_awaitable);
            if (ec){
                std::cout << "[StreamStatuses] Channel error or closed: " << ec.message() << "\n";
                break;
            }

            fms::RobotStatusUpdate status_update;
            status_update.set_robot_id(msg_status.robot_id);
            status_update.set_status(msg_status.status);
            status_update.set_operating_mode(msg_status.operating_mode);
            status_update.set_timestamp(msg_status.timestamp);
            status_update.set_driving(msg_status.driving);

            auto* battery = status_update.mutable_battery_state();
            battery->set_battery_charge(msg_status.battery_state.battery_charge);
            battery->set_charging(msg_status.battery_state.charging);

            bool ok = co_await rpc.write(status_update);
            if (!ok) {
                std::cout << "[StreamStatuses] Client disconnected or stream closed.\n";
                // status_robot.close();
                break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[StreamStatuses] Error: " << e.what() << "\n";
    }

    co_await rpc.finish(grpc::Status::OK);
}


asio::awaitable<void> GrpcDispatchService::process_stream_visualizations(VisualizationsStreamRPC& rpc, const fms::StreamStatusesRequest& request) {
    std::cout << "[StreamVisualizations] Client connected: " << request.client_id() << "\n";
    if (!visu_robot.is_open()) {
        visu_robot = stream_visualisation_channel{grpc_context.get_executor(), 50};
    }
    try {

        while (true) {
            auto [ec, msg_visu] = co_await visu_robot.async_receive(use_nothrow_awaitable);

            if (ec){
                std::cout << "[StreamStatuses] Channel error or closed: " << ec.message() << "\n";
                break;
            }
            fms::RobotVisualization visual_update;
            visual_update.set_robot_id(msg_visu.robot_id);

            auto* pos = visual_update.mutable_agv_position();
            pos->set_map_id(msg_visu.agv_position.map_id);
            pos->set_x(msg_visu.agv_position.x);
            pos->set_y(msg_visu.agv_position.y);
            pos->set_theta(msg_visu.agv_position.theta);
            pos->set_position_initialized(msg_visu.agv_position.position_initialized);

            bool ok = co_await rpc.write(visual_update);
            std::println("Received message : [ROBOT_ID] - {}, MAP_ID {}", msg_visu.robot_id, msg_visu.agv_position.map_id);
            if (!ok) {
               // visu_robot.close();
                std::cout << "[StreamVisualizations] Client disconnected.\n";
                break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[StreamVisualizations] Error: " << e.what() << "\n";
    }

    co_await rpc.finish(grpc::Status::OK);
}

asio::awaitable<std::optional<RegisterRobot>> GrpcDispatchService::get_register_robot(){
    auto [ec, msg] = co_await register_robot.async_receive(use_nothrow_awaitable);
    if (ec) co_return std::nullopt;

    co_return std::move(msg);
}

asio::awaitable<std::optional<UnregisterRobot>> GrpcDispatchService::get_unregister_robot(){
    auto [ec, msg] = co_await unregister_robot.async_receive(use_nothrow_awaitable);
    if (ec) co_return std::nullopt;

    co_return std::move(msg);
}



void GrpcDispatchService::set_status_robot(RobotStatusUpdate& data){
    if (!status_robot.is_open()) {
        return;
    }

    boost::system::error_code ec;
    status_robot.try_send(ec, std::move(data));

    if (ec) {
        std::cerr << "[MQTT->Channel] Failed to send, buffer full: " << ec.message() << "\n";
    }
}


void GrpcDispatchService::set_visu_robot(RobotVisualization& data){
    if (!visu_robot.is_open()) {
        return;
    }

    boost::system::error_code ec;
    visu_robot.try_send(ec, std::move(data));

    if (ec) {
        std::cerr << "[MQTT->Channel] Failed to send, buffer full: " << ec.message() << "\n";
    }
}