#include <agrpc/asio_grpc.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <cstdlib>
#include <string>

#include "Database.hpp"
#include "DatabaseService.hpp"
#include "GrpcServer.hpp"
#include "spdlog/spdlog.h"
#include "RestApi.hpp"

std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

ip::tcp::endpoint endpoint_from(const std::string& address) {
    auto colon = address.rfind(':');
    return {ip::make_address(address.substr(0, colon)),
            static_cast<unsigned short>(std::stoi(address.substr(colon + 1)))};
}

int main() {
    try {
        const std::string database_url = env_or("DATABASE_URL", "postgresql://fms:fms@127.0.0.1:5432/fms");
        const std::string grpc_address = env_or("GRPC_ADDRESS", "0.0.0.0:5002");
        const std::string migrations_dir = env_or("MIGRATIONS_DIR", FMS_MIGRATIONS_DIR);
        const std::string http_address = env_or("HTTP_ADDRESS", "0.0.0.0:8002");

        Database database{database_url};
        database.migrate(migrations_dir);
        database.start();

        asio::io_context io_context{1};
        GrpcServer grpc_server{grpc_address};
        agrpc::GrpcContext grpc_context{grpc_server.add_completion_queue()};

        DatabaseService database_service{grpc_context, database};
        grpc_server.add_service(database_service.registry_service());
        grpc_server.add_service(database_service.storage_service());

        ApiRouter router;
        RestApi rest_api{database, router};
        HttpServer http_server{endpoint_from(http_address), io_context, router};

        grpc_server.start();
        database_service.start();
        http_server.start();
        grpc_server.set_serving(true);

        asio::signal_set signals{io_context, SIGINT, SIGTERM};
        signals.async_wait([&](const boost::system::error_code&, int signal) {
            spdlog::info("signal {} received, stopping", signal);
            grpc_server.shutdown();
        });

        spdlog::info("fleet_database started: gRPC {}, HTTP {}", grpc_address, http_address);
        agrpc::run(grpc_context, io_context, [&] { return grpc_context.is_stopped(); });

        grpc_server.destroy();
        database.stop();
        http_server.stop();
        spdlog::info("fleet_database stopped");
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        spdlog::error("fleet_database failed: {}", e.what());
        return EXIT_FAILURE;
    }
}