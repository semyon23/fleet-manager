//
// Created by ubuntu on 10/17/26.
//

#include "../include/GrpcServer.hpp"
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "spdlog/spdlog.h"
// GrpcServer.cpp
#include "../include/GrpcServer.hpp"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>
#include <stdexcept>

#include "fleet_dispatch.grpc.pb.h"
#include "spdlog/spdlog.h"

GrpcServer::GrpcServer(std::string address) : address_(std::move(address)) {
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    builder_.emplace();
    builder_->AddListeningPort(address_, grpc::InsecureServerCredentials());
}

std::unique_ptr<grpc::ServerCompletionQueue> GrpcServer::add_completion_queue() {
    return builder_->AddCompletionQueue();
}

void GrpcServer::add_service(grpc::Service& service) {
    builder_->RegisterService(&service);
}

void GrpcServer::start() {
    server_ = builder_->BuildAndStart();
    if (!server_) {
        throw std::runtime_error("gRPC server failed to start on " + address_);
    }
    set_serving(false);   // готов станет, когда DispatchApp всё восстановит
    spdlog::info("[grpc_server] listening on {}", address_);
}

void GrpcServer::set_serving(bool serving) {
    auto* health = server_->GetHealthCheckService();
    health->SetServingStatus("", serving);
    health->SetServingStatus(fms::dispatch::v1::FleetDispatch::service_full_name(), serving);
}

void GrpcServer::shutdown() {
    set_serving(false);
    shutdown_thread_ = std::thread([this] {
        server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
        spdlog::info("[grpc_server] stopped");
    });
}

void GrpcServer::destroy() {
    if (shutdown_thread_.joinable()) {
        shutdown_thread_.join();
    }
    server_.reset();
}