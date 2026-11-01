#include <agrpc/asio_grpc.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <cstdlib>
#include <string>
#include "GrpcServer.hpp"
#include "http_server.hpp"
#include "spdlog/spdlog.h"
#include "env_config.hpp"

namespace asio  = boost::asio;

int main() {
    const std::string grpc_address = env_or("GRPC_ADDRESS", "0.0.0.0:5003");
    const std::string http_address = env_or("HTTP_ADDRESS", "0.0.0.0:8003");

    asio::io_context io_context{1};
    GrpcServer grpc_server{grpc_address};
    agrpc::GrpcContext grpc_context{grpc_server.add_completion_queue()};

    ApiRouter router;
   // RestApi rest_api{database, router};
    HttpServer http_server{endpoint_from(http_address), io_context, router};

    grpc_server.start();
    http_server.start();
    grpc_server.set_serving(true);


    asio::signal_set signals{io_context, SIGINT, SIGTERM};
    signals.async_wait([&](const boost::system::error_code&, int signal) {
        spdlog::info("signal {} received, stopping", signal);
        grpc_server.shutdown();
    });

    spdlog::info("fleet_control started: gRPC {}, HTTP {}", grpc_address, http_address);
    agrpc::run(grpc_context, io_context, [&] { return grpc_context.is_stopped(); });

    grpc_server.destroy();

    http_server.stop();
    spdlog::info("[fleet control] stopped");


    return EXIT_SUCCESS;
}
