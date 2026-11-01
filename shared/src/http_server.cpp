#include "../include/http_server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>
#include <ranges>
#include <spdlog/spdlog.h>

namespace {

constexpr auto kTimeout = std::chrono::seconds(15);

void apply_cors_headers(HttpResponse& res) {
    res.set(http::field::access_control_allow_origin, "*");
    res.set(http::field::access_control_allow_methods, "GET, POST, PATCH, DELETE, OPTIONS");
    res.set(http::field::access_control_allow_headers, "Content-Type, Authorization");
    res.set(http::field::access_control_max_age, "86400");
}

// "/api/fms/robots/amr-01?x=1" → {"api", "fms", "robots", "amr-01"}; query в поиске не участвует
std::vector<std::string> split_path(std::string_view target) {
    target = target.substr(0, target.find('?'));
    std::vector<std::string> segments;
    for (auto part : std::views::split(target, '/')) {
        if (!part.empty()) {
            segments.emplace_back(part.begin(), part.end());
        }
    }
    return segments;
}

bool is_param(const std::string& segment) {
    return segment.size() > 2 && segment.front() == '{' && segment.back() == '}';
}

}  // namespace


HttpResponse json_response(const HttpRequest& req, http::status status, std::string body) {
    HttpResponse res{status, req.version()};
    res.set(http::field::content_type, "application/json");
    res.body() = std::move(body);
    return res;
}

HttpResponse error_response(const HttpRequest& req, http::status status, std::string_view code, std::string_view message) {
    nlohmann::json body{{"code", code}, {"message", message}, {"details", nlohmann::json::object()}};
    return json_response(req, status, body.dump());
}

std::optional<std::string> query_param(const HttpRequest& req, std::string_view name) {
    std::string_view target = req.target();
    auto question = target.find('?');
    if (question == std::string_view::npos) {
        return std::nullopt;
    }
    for (auto part : std::views::split(target.substr(question + 1), '&')) {
        std::string_view pair(part.begin(), part.end());
        auto equals = pair.find('=');
        if (pair.substr(0, equals) == name) {
            return equals == std::string_view::npos ? std::string{} : std::string(pair.substr(equals + 1));
        }
    }
    return std::nullopt;
}

// ==================================================================== ApiRouter

void ApiRouter::add_route(http::verb method, std::string_view pattern, RouteCallback callback) {
    routes_.push_back({method, split_path(pattern), std::move(callback)});
    spdlog::debug("[api_router] registry endpoint - {} {}", http::to_string(method), pattern);
}

asio::awaitable<HttpResponse> ApiRouter::route_request(const HttpRequest& req) {
    auto segments = split_path(req.target());
    for (const auto& route : routes_) {
        if (route.method != req.method() || route.segments.size() != segments.size()) {
            continue;
        }
        PathParams params;
        bool matched = true;
        for (std::size_t i = 0; i < segments.size() && matched; ++i) {
            if (is_param(route.segments[i])) {
                params[route.segments[i].substr(1, route.segments[i].size() - 2)] = segments[i];
            } else {
                matched = route.segments[i] == segments[i];
            }
        }
        if (matched) {
            co_return co_await route.callback(req, params);
        }
    }
    co_return error_response(req, http::status::not_found, "NOT_FOUND", "no such endpoint");
}

// ==================================================================== HttpServer

HttpServer::HttpServer(ip::tcp::endpoint endpoint, asio::io_context& io, ApiRouter& router)
    : ioc_(io), endpoint_(endpoint), acceptor_(io), router_(router) {}

void HttpServer::start() {
    acceptor_.open(endpoint_.protocol());
    acceptor_.set_option(ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint_);
    acceptor_.listen();
    asio::co_spawn(ioc_, do_listen(), asio::detached);
    spdlog::info("[http_server] listening on {}:{}", endpoint_.address().to_string(), endpoint_.port());
}

void HttpServer::stop() {
    boost::system::error_code ec;
    acceptor_.close(ec);
}

asio::awaitable<void> HttpServer::do_listen() {
    while (acceptor_.is_open()) {
        auto [ec, socket] = co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted) {
            break;   // stop()
        }
        if (ec) {
            spdlog::error("[http_server] accept failed: {}", ec.message());
            asio::steady_timer pause(ioc_, std::chrono::milliseconds(100));
            co_await pause.async_wait(asio::as_tuple(asio::use_awaitable));
            continue;
        }
        asio::co_spawn(ioc_, do_session(beast::tcp_stream(std::move(socket))), asio::detached);
    }
}

asio::awaitable<void> HttpServer::do_session(beast::tcp_stream stream) {
    beast::error_code ec;
    auto remote = stream.socket().remote_endpoint(ec);
    std::string client = ec ? "unknown" : remote.address().to_string() + ":" + std::to_string(remote.port());

    beast::flat_buffer buffer;
    try {
        while (true) {
            stream.expires_after(kTimeout);
            HttpRequest req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            HttpResponse res = co_await handle_request(req);

            stream.expires_after(kTimeout);   // на запись — свой срок
            co_await http::async_write(stream, res, asio::use_awaitable);
            if (!res.keep_alive()) {
                break;
            }
        }
    } catch (const boost::system::system_error& se) {
        if (se.code() == beast::error::timeout) {
            spdlog::debug("[http_server] {} idle for {} s, connection closed", client, kTimeout.count());
        } else if (se.code() != http::error::end_of_stream) {
            spdlog::error("[http_server] {}: {}", client, se.what());
        }
    }
    stream.socket().shutdown(ip::tcp::socket::shutdown_send, ec);
}

asio::awaitable<HttpResponse> HttpServer::handle_request(const HttpRequest& req) {
    spdlog::debug("[http_server] {} {}", req.method_string(), req.target());
    HttpResponse res;
    if (req.method() == http::verb::options) {
        res = HttpResponse{http::status::no_content, req.version()};   // preflight браузера
    } else {
        try {
            res = co_await router_.route_request(req);
        } catch (const std::exception& e) {
            spdlog::error("[http_server] {} {} failed: {}", req.method_string(), req.target(), e.what());
            res = error_response(req, http::status::internal_server_error, "INTERNAL", "internal error");
        }
    }
    apply_cors_headers(res);
    res.keep_alive(req.keep_alive());
    res.prepare_payload();
    co_return res;
}