#include <agrpc/asio_grpc.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "alarms/AlarmEngine.hpp"
#include "journal/Journal.hpp"
#include "journal/RobotEvents.hpp"
#include "api/HealthApi.hpp"
#include "api/MapApi.hpp"
#include "api/MissionApi.hpp"
#include "api/RobotApi.hpp"
#include "api/RouteApi.hpp"
#include "api/AlarmApi.hpp"
#include "assignment/MissionAssigner.hpp"
#include "env_config.hpp"
#include "fleet/FleetSync.hpp"
#include "http_server.hpp"
#include "maps/MapStore.hpp"
#include "missions/MissionStore.hpp"
#include "missions/MissionRunner.hpp"
#include "services/Services.hpp"
#include "spdlog/spdlog.h"

int main() {
    try {
        const std::string http_address = env_or("HTTP_ADDRESS", "0.0.0.0:8003");
        const std::string database_address = env_or("DATABASE_ADDRESS", "127.0.0.1:5002");
        const std::string dispatch_address = env_or("DISPATCH_ADDRESS", "127.0.0.1:5001");
        const double low_battery_percent = std::stod(env_or("LOW_BATTERY_PERCENT", "15"));

        const double default_speed = std::stod(env_or("FMS_DEFAULT_SPEED", "1.0"));
        if (default_speed <= 0) {
            throw std::runtime_error("FMS_DEFAULT_SPEED must be > 0");
        }

        asio::io_context io_context{1};
        agrpc::GrpcContext grpc_context;
        auto grpc_work = asio::make_work_guard(grpc_context);

        Services services{grpc_context, database_address, dispatch_address};
        MapStore maps{io_context, services, default_speed};
        MissionStore missions{io_context, services};
        AvailabilityTracker availability{io_context, services, maps, low_battery_percent};
        Journal journal{io_context, services};
        AlarmEngine alarm_engine{io_context, services, low_battery_percent};


        availability.on_status_change = [&](const std::string& robot_id, const fms::v1::RobotStatus* before,
                                            const fms::v1::RobotStatus& after) {
            for (const auto& event : robot_status_events(before, after)) {
                journal.add("ROBOT", event, robot_id);
            }
            alarm_engine.on_robot(robot_id, &after);
        };
        availability.on_availability = [&](const std::string& robot_id, const fms::database::v1::RobotAvailability* before,
                                           const fms::database::v1::RobotAvailability& after) {
            if (auto event = availability_event(before, after)) {
                journal.add("ROBOT", *event, robot_id);
            }
        };
        availability.on_robot_removed = [&](const std::string& robot_id) { alarm_engine.remove_robot(robot_id); };
        availability.on_keep_only = [&](const std::set<std::string>& robot_ids) { alarm_engine.keep_only(robot_ids); };
        availability.on_dispatch = [&](bool connected) { alarm_engine.set_dispatch_connected(connected); };

        MissionRunner runner{io_context, services, missions, maps};
        runner.configure_traffic(availability);


        MissionAssigner assigner{io_context, missions, availability, maps, runner};
        missions.on_queue_change = [&] { assigner.wake(); };
        availability.on_idle = [&] { assigner.wake(); };
        runner.on_checkpoint_jobs_drained = [&] { assigner.wake(); };
        maps.on_change = [&] {
            availability.recompute_all();
            assigner.wake();
        };

        FleetSync fleet_sync{io_context, services, availability};
        ApiRouter router;
        HealthApi health_api{services, router};
        RobotApi robot_api{services, fleet_sync, availability, maps, journal, router};
        MapApi map_api{services, maps, journal, router, &runner};
        AlarmApi alarm_api{alarm_engine, router};
        MissionApi mission_api{services, maps, missions, runner, router};
        RouteApi route_api{runner, router};
        HttpServer http_server{endpoint_from(http_address), io_context, router};

        http_server.start();
        availability.start();
        fleet_sync.start();
        maps.start();
        missions.start();
        assigner.start();
        runner.start();
        journal.start();
        alarm_engine.start();

        asio::signal_set signals{io_context, SIGINT, SIGTERM};
        signals.async_wait([&](const boost::system::error_code&, int signal) {
            spdlog::info("signal {} received, stopping", signal);
            http_server.stop();
            assigner.stop();
            runner.stop();
            fleet_sync.stop();
            maps.stop();
            missions.stop();
            availability.stop();
            alarm_engine.stop();
            journal.stop();
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