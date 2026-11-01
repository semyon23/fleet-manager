#include "../include/HealthApi.hpp"
#include <boost/asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

using HealthCheckRPC = agrpc::ClientRPC<&grpc::health::v1::Health::Stub::PrepareAsyncCheck>;

namespace {

    asio::awaitable<std::string> check(agrpc::GrpcContext& grpc_context, grpc::health::v1::Health::Stub& stub) {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
        grpc::health::v1::HealthCheckRequest request;
        grpc::health::v1::HealthCheckResponse response;
        grpc::Status status = co_await HealthCheckRPC::request(grpc_context, stub, context, request, response,
                                                               asio::use_awaitable);
        bool serving = status.ok() && response.status() == grpc::health::v1::HealthCheckResponse::SERVING;
        co_return serving ? "OK" : "DOWN";
    }

}  // namespace

HealthApi::HealthApi(Services& services, ApiRouter& router) : services_(services) {
    router.add_route(http::verb::get, "/api/health",
        [this](const HttpRequest& req, const PathParams&) { return health(req); });
}

asio::awaitable<HttpResponse> HealthApi::health(const HttpRequest& req) {
    std::string database = co_await check(services_.grpc_context, *services_.database_health);
    std::string dispatch = co_await check(services_.grpc_context, *services_.dispatch_health);

    nlohmann::json body{
        {"status", database == "OK" && dispatch == "OK" ? "OK" : "DEGRADED"},
        {"services", {{"control", "OK"}, {"database", database}, {"dispatch", dispatch}}},
    };
    co_return json_response(req, http::status::ok, body.dump());
}