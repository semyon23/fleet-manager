#include <boost/asio/signal_set.hpp>
#include "include/MqttClientHandler.hpp"
#include "include/GrpcDispatchService.hpp"
#include <grpcpp/server_builder.h>
#include "include/DispatcherServer.hpp"

int main() {

    asio::io_context ioc{1};
    grpc::ServerBuilder builder;

    std::unique_ptr<grpc::ServerCompletionQueue> cq = builder.AddCompletionQueue();
    agrpc::GrpcContext grpc_context{std::move(cq)};

    auto DispatchServer = std::make_shared<GrpcDispatchService>(builder, grpc_context, "0.0.0.0:5001");
    DispatchServer->start(builder);

    MqttClientConfig config;
    MqttClientHandler mqtt_client(ioc, config);
    mqtt_client.start();

    auto FleetStream = std::make_shared<DispatcherServer>(mqtt_client, DispatchServer);

    FleetStream->run(ioc);

    asio::signal_set signals(ioc, SIGINT, SIGTERM);

    signals.async_wait([&](auto, int) {
        DispatchServer->stop();
        mqtt_client.stop();
        grpc_context.stop();
        ioc.stop();
    });

    asio::post(ioc, [&] {
        agrpc::run(grpc_context, ioc, [&] {
            return grpc_context.is_stopped();
        });
    });

    ioc.run();

    DispatchServer.reset();

    return EXIT_SUCCESS;
}
