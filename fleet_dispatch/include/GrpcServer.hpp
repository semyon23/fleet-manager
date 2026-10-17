
#ifndef GRPCSERVER_HPP
#define GRPCSERVER_HPP

#include <string>
#include <memory>
#include <agrpc/asio_grpc.hpp>
#include <grpcpp/server_builder.h>
#include <grpcpp/server.h>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <optional>
#include <string>
#include <thread>

class GrpcServer {
public:
    explicit GrpcServer(std::string address);

    std::unique_ptr<grpc::ServerCompletionQueue> add_completion_queue();
    void add_service(grpc::Service& service);
    void start();
    void set_serving(bool serving);
    void shutdown();
    void destroy();

private:
    std::string address_;
    std::optional<grpc::ServerBuilder> builder_;
    std::unique_ptr<grpc::Server> server_;
    std::thread shutdown_thread_;
};


#endif //GRPCSERVER_HPP
