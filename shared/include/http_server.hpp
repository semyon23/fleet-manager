#ifndef FLEETMANAGERSYSTEM_HTTP_SERVER_HPP
#define FLEETMANAGERSYSTEM_HTTP_SERVER_HPP
#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
namespace ip    = asio::ip;

using HttpRequest   = http::request<http::string_body>;
using HttpResponse  = http::response<http::string_body>;
using PathParams    = std::unordered_map<std::string, std::string>;   // "{id}" из шаблона → значение из пути
using RouteCallback = std::function<asio::awaitable<HttpResponse>(const HttpRequest&, const PathParams&)>;

// Ответы для обработчиков. CORS, keep-alive и Content-Length добавляет сервер.
HttpResponse json_response(const HttpRequest& req, http::status status, std::string body);
// Ошибка по контракту UI: {"code": ..., "message": ..., "details": {}}
HttpResponse error_response(const HttpRequest& req, http::status status, std::string_view code, std::string_view message);

// Маршруты сервиса: метод + шаблон пути ("/api/fms/robots/{id}") → обработчик
class ApiRouter {
public:
    void add_route(http::verb method, std::string_view pattern, RouteCallback callback);
    asio::awaitable<HttpResponse> route_request(const HttpRequest& req);   // нет маршрута — 404

private:
    struct Route {
        http::verb method;
        std::vector<std::string> segments;   // "{id}" совпадает с любым сегментом
        RouteCallback callback;
    };
    std::vector<Route> routes_;   // перебор по порядку: маршрутов немного, ключ map здесь не помогает
};

// Транспорт: принять соединение, прочитать запрос, отдать роутеру, записать ответ
class HttpServer {
public:
    HttpServer(ip::tcp::endpoint endpoint, asio::io_context& io, ApiRouter& router);
    void start();
    void stop();

private:
    asio::awaitable<void> do_listen();
    asio::awaitable<void> do_session(beast::tcp_stream stream);
    asio::awaitable<HttpResponse> handle_request(const HttpRequest& req);

    asio::io_context& ioc_;
    ip::tcp::endpoint endpoint_;
    ip::tcp::acceptor acceptor_;
    ApiRouter& router_;
};

#endif //FLEETMANAGERSYSTEM_HTTP_SERVER_HPP
