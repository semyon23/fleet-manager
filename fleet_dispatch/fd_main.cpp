#include <agrpc/asio_grpc.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include "DispatchService.hpp"
#include "GrpcServer.hpp"
#include "RobotMessageRouter.hpp"
#include "spdlog/spdlog.h"

int main(){
    try{
        asio::io_context io_context{1};

        MqttClientConfig mqtt_config;
        mqtt_config.host = "127.0.0.1";
        mqtt_config.port = 1884;
        mqtt_config.client_id = "fleet_dispatch";
        MqttClientHandler mqtt{io_context, mqtt_config};
        Outbox outbox{io_context, 1000};

        RobotStatusHub status_hub{io_context};
        MissionStatusHub mission_hub{io_context};

        GrpcServer grpc_server{"0.0.0.0:5001"};
        agrpc::GrpcContext grpc_context{grpc_server.add_completion_queue()};

        FleetRegistry registry{io_context, outbox, status_hub, mission_hub};
        RobotMessageRouter message_router{io_context, mqtt, registry, outbox};
        DispatchService dispatch_service{grpc_context, registry, status_hub, mission_hub};
        grpc_server.add_service(dispatch_service.grpc_service());


        mqtt.start();
        message_router.start();
        grpc_server.start();
        dispatch_service.start();
        grpc_server.set_serving(true);


        asio::signal_set signals{io_context, SIGINT, SIGTERM};
        signals.async_wait([&](const boost::system::error_code&, int signal)
        {
            spdlog::info("signal {} received, stopping", signal);
            grpc_server.shutdown();
            status_hub.close_all();
            mission_hub.close_all();
            registry.stop_all();
            outbox.close();
            mqtt.stop();
        });


        spdlog::info("fleet_dispatch started");
        agrpc::run(grpc_context, io_context, [&] { return grpc_context.is_stopped(); });


        grpc_server.destroy();
        spdlog::info("fleet_dispatch stopped");
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        spdlog::error("fleet_dispatch failed: {}", e.what());
        return EXIT_FAILURE;
    }
}
