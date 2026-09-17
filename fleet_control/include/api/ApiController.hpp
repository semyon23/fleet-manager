#pragma once
#ifndef APICONTROLLER_HPP
#define APICONTROLLER_HPP

class MissionsService;
class RobotsService;

#include <memory>
#include <boost/beast/http.hpp>
#include "include/api/RobotApiHandler.hpp"

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;

using  RouteCallback = std::function<asio::awaitable<http::message_generator>(http::request<http::string_body>&&)>;


class ApiController {
public:
    ApiController(std::shared_ptr<RobotApiHandler> robot);
    void add_route(http::verb type, std::string route, RouteCallback callback);
    asio::awaitable<http::message_generator> route_request(http::request<http::string_body>&& req);
private:
    asio::awaitable<http::message_generator> get_health(http::request<http::string_body>&& req);
    std::unordered_map<std::string, RouteCallback> routes;

    std::shared_ptr<RobotApiHandler> robot_api;
};



#endif //APICONTROLLER_HPP
