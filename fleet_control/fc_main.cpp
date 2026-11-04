#include <agrpc/asio_grpc.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <cstdlib>
#include <string>

#include "api/HealthApi.hpp"
#include "api/RobotApi.hpp"
#include "api/MapApi.hpp"
#include "services//Services.hpp"
#include "fleet//FleetSync.hpp"
#include "env_config.hpp"
#include "http_server.hpp"
#include "maps/MapStore.hpp"
#include "spdlog/spdlog.h"

int main() {
    try {
        const std::string http_address = env_or("HTTP_ADDRESS", "0.0.0.0:8003");
        const std::string database_address = env_or("DATABASE_ADDRESS", "127.0.0.1:5002");
        const std::string dispatch_address = env_or("DISPATCH_ADDRESS", "127.0.0.1:5001");
        const double low_battery_percent = std::stod(env_or("LOW_BATTERY_PERCENT", "15"));
        asio::io_context io_context{1};
        agrpc::GrpcContext grpc_context;
        auto grpc_work = asio::make_work_guard(grpc_context);

        Services services{grpc_context, database_address, dispatch_address};
        AvailabilityTracker availability{io_context, services, low_battery_percent};
        FleetSync fleet_sync{io_context, services, availability};
        MapStore maps{io_context, services};
        ApiRouter router;
        HealthApi health_api{services, router};
        RobotApi robot_api{services, fleet_sync, availability, router};
        MapApi map_api{services, maps, router};
        HttpServer http_server{endpoint_from(http_address), io_context, router};

        http_server.start();
        availability.start();
        fleet_sync.start();
        maps.start();


        asio::signal_set signals{io_context, SIGINT, SIGTERM};
        signals.async_wait([&](const boost::system::error_code&, int signal) {
            spdlog::info("signal {} received, stopping", signal);
            http_server.stop();
            fleet_sync.stop();
            maps.stop();
            availability.stop();
            grpc_work.reset();
        });

        spdlog::info("fleet_control started: HTTP {}; database {}, dispatch {}",
                     http_address, database_address, dispatch_address);
        agrpc::run(grpc_context, io_context, [&] { return grpc_context.is_stopped(); });

        spdlog::info("fleet_control stopped");
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        spdlog::error("fleet_control failed: {}", e.what());
        return EXIT_FAILURE;
    }
}