#include <boost/asio/signal_set.hpp>
#include "include/MqttClientHandler.hpp"
#include "include/GrpcServerHandler.hpp"
#include <grpcpp/server_builder.h>
#include "include/FleetStreamGateway.hpp"

int main() {

    asio::io_context ioc{1};
    grpc::ServerBuilder builder;

    std::unique_ptr<grpc::ServerCompletionQueue> cq = builder.AddCompletionQueue();
    agrpc::GrpcContext grpc_context{std::move(cq)};

    auto server_handler = std::make_shared<GrpcServerHandler>(builder, grpc_context, "0.0.0.0:5001");
    server_handler->start(builder);

    MqttClientConfig config;
    MqttClientHandler client(ioc, config);
    client.start();

    auto FleetStream = std::make_shared<FleetStreamGateway>(client, server_handler);

    FleetStream->run(ioc);

    asio::signal_set signals(ioc, SIGINT, SIGTERM);

    signals.async_wait([&](auto, int) {
        server_handler->stop();
        client.stop();
        grpc_context.stop();
        ioc.stop();
    });

    asio::post(ioc, [&] {
        agrpc::run(grpc_context, ioc, [&] {
            return grpc_context.is_stopped();
        });
    });

    ioc.run();

    server_handler.reset();

    return EXIT_SUCCESS;
}
