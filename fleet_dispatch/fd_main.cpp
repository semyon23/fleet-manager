#include <agrpc/asio_grpc.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "include/DispatchService.hpp"
#include "GrpcServer.hpp"
#include "include/RobotMessageRouter.hpp"
#include "env_config.hpp"
#include "spdlog/spdlog.h"

int main(){
    try{
        asio::io_context io_context{1};

        MqttClientConfig mqtt_config;
        mqtt_config.host = env_or("MQTT_HOST", "127.0.0.1");
        mqtt_config.port = static_cast<uint>(std::stoi(env_or("MQTT_PORT", "1883")));
        mqtt_config.client_id = "fleet_dispatch";
        mqtt_config.username = env_or("MQTT_USER", "");
        mqtt_config.password = env_or("MQTT_PASSWORD", "");

        MqttClientHandler mqtt{io_context, mqtt_config};
        Outbox outbox{io_context, 1000};

        RobotStatusHub status_hub{io_context};
        MissionStatusHub mission_hub{io_context};

        GrpcServer grpc_server{env_or("DISPATCH_LISTEN_ADDRESS", "0.0.0.0:5001")};
        agrpc::GrpcContext grpc_context{grpc_server.add_completion_queue()};

        const char* database_address = std::getenv("DATABASE_ADDRESS");
        DatabaseWriter database{grpc_context, io_context, database_address ? database_address : "127.0.0.1:5002"};


        FleetRegistry registry{io_context, outbox, status_hub, mission_hub, database};
        RobotMessageRouter message_router{io_context, mqtt, registry, outbox};
        DispatchService dispatch_service{grpc_context, registry, status_hub, mission_hub, database};
        grpc_server.add_service(dispatch_service.grpc_service());


        mqtt.start();
        message_router.start();
        database.start();
        grpc_server.start();
        dispatch_service.start();
        bool stopping = false;
        asio::steady_timer retry(io_context);
        asio::co_spawn(io_context, [&]() -> asio::awaitable<void> {
            while (!stopping) {
                fms::database::v1::LoadDispatchSnapshotResponse response;
                auto status = co_await database.load_snapshot(response);
                if (stopping) co_return;
                if (status.ok() && response.has_snapshot()) {
                    std::optional<StartupInventory> inventory;
                    try {
                        inventory.emplace(std::move(*response.mutable_snapshot()));
                    } catch (const std::invalid_argument& error) {
                        spdlog::error("[startup] {}", error.what());
                    }
                    if (inventory) {
                        co_await registry.restore(std::move(*inventory));
                        if (stopping) co_return;
                        const auto& reason = registry.inventory()->reason;
                        grpc_server.set_serving(reason.empty());
                        spdlog::info("[startup] inventory loaded: {}", reason.empty() ? "ready" : reason);
                        co_return;
                    }
                } else {
                    spdlog::warn("[startup] snapshot unavailable: {}", status.error_message());
                }
                retry.expires_after(std::chrono::seconds(2));
                co_await retry.async_wait(asio::as_tuple(asio::use_awaitable));
            }
        }, [&](std::exception_ptr error) {
            if (!error || stopping) return;
            try { std::rethrow_exception(error); }
            catch (const std::exception& e) { spdlog::critical("[startup] {}", e.what()); }
        });


        asio::signal_set signals{io_context, SIGINT, SIGTERM};
        signals.async_wait([&](const boost::system::error_code&, int signal)
        {
            spdlog::info("signal {} received, stopping", signal);
            stopping = true;
            retry.cancel();
            grpc_server.shutdown();
            status_hub.close_all();
            mission_hub.close_all();
            database.stop();
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
