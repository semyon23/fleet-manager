#include <agrpc/asio_grpc.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include "DispatchService.hpp"
#include "GrpcServer.hpp"
#include "spdlog/spdlog.h"

int main() {
    try {
        asio::io_context io_context{1};
        GrpcServer grpc_server{"0.0.0.0:5001"};
        agrpc::GrpcContext grpc_context{grpc_server.add_completion_queue()};

        FleetRegistry registry{io_context};
        DispatchService dispatch_service{grpc_context, registry};
        grpc_server.add_service(dispatch_service.grpc_service());

        grpc_server.start();
        dispatch_service.start();
        grpc_server.set_serving(true);

        asio::signal_set signals{io_context, SIGINT, SIGTERM};
        signals.async_wait([&](const boost::system::error_code&, int signal) {
            spdlog::info("signal {} received, stopping", signal);
            registry.stop_all();
            grpc_server.shutdown();
        });

        agrpc::run(grpc_context, io_context, [&] { return grpc_context.is_stopped(); });

        grpc_server.destroy();
        spdlog::info("fleet_dispatch stopped");
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("fleet_dispatch failed: {}", e.what());   // например, порт занят
        return 1;
    }
    //
    // asio::io_context ioc{1};
    //
    //
    // agrpc::GrpcContext grpc_context{std::move(cq)};
    //
    // auto DispatchServer = std::make_shared<GrpcDispatchService>(builder, grpc_context, "0.0.0.0:5001");
    // DispatchServer->start(builder);
    //
    //
    //
    //
    //
    //
    //
    // asio::signal_set signals(ioc, SIGINT, SIGTERM);
    //
    // signals.async_wait([&](auto, int) {
    //     DispatchServer->stop();
    //     grpc_context.stop();
    //     ioc.stop();
    // });
    //
    // asio::post(ioc, [&] {
    //     agrpc::run(grpc_context, ioc, [&] {
    //         return grpc_context.is_stopped();
    //     });
    // });
    //
    // ioc.run();



    return EXIT_SUCCESS;
}
