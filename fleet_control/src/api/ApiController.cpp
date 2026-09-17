#include "include/api/ApiController.hpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <boost/beast/http/string_body.hpp>
#include <print>

using json = nlohmann::json;

ApiController::ApiController(std::shared_ptr<RobotApiHandler> robot)
    : robot_api(std::move(robot)){

    add_route(http::verb::get, "/api/health",
     [this](http::request<http::string_body>&& req) -> asio::awaitable<http::message_generator>{
        return this->get_health(std::move(req));
     });

     add_route(http::verb::post, "/api/fms/robots",
         [this](http::request<http::string_body>&& req)->asio::awaitable<http::message_generator> {
        return this->robot_api->onCreateRobot(std::move(req));
     });

    add_route(http::verb::delete_, "/api/fms/robots",
          [this](http::request<http::string_body>&& req)->asio::awaitable<http::message_generator>{
         return this->robot_api->onDeleteRobot(std::move(req));
    });

    add_route(http::verb::get, "/api/fms/robots",
       [this](http::request<http::string_body>&& req)->asio::awaitable<http::message_generator>{
        return this->robot_api->getAllRobots(std::move(req));
    });

}

void ApiController::add_route(http::verb type, std::string route, RouteCallback callback){
    std::string method_str = std::string(http::to_string(type));
    std::string route_key = method_str + ":" + route;
    routes[route_key] = std::move(callback);
    std::cout << "[ROUTER] Registered endpoint: " << route_key << "\n";
}


inline void apply_cors_headers(http::response<http::string_body>& res) {
    res.set(http::field::access_control_allow_origin, "*");
    res.set(http::field::access_control_allow_methods, "GET, POST, PATCH, DELETE, OPTIONS");
    res.set(http::field::access_control_allow_headers, "Content-Type, Authorization");
    res.set(http::field::access_control_max_age, "86400");
}

asio::awaitable<http::message_generator> ApiController::route_request(http::request<http::string_body>&& req){

    auto method_str = std::string(http::to_string(req.method()));
    auto target_str = std::string(req.target());
    auto route_key = method_str + ":" + target_str;

    auto it = routes.find(route_key);

    if (it != routes.end()) {
        auto msg = co_await it->second(std::move(req));
        co_return msg;
    }

    http::response<http::string_body> res{http::status::not_found, req.version()};
    res.set(http::field::server, "FleetManagerSystem/1.0");
    res.set(http::field::content_type, "application/json");


    apply_cors_headers(res);

    res.keep_alive(req.keep_alive());
    res.body() = R"({"error": "Not Found", "message": "Requested endpoint does not exist"})";
    res.prepare_payload();
    co_return res;
}


asio::awaitable<http::message_generator> ApiController::get_health(http::request<http::string_body>&& req){
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::content_type, "text/plain");

    res.set(http::field::access_control_allow_origin, "*");
    res.set(http::field::access_control_allow_methods, "GET, OPTIONS");
    res.set(http::field::access_control_allow_headers, "Content-Type");

    res.body() = "OK";
    res.prepare_payload();

    co_return res;
}
