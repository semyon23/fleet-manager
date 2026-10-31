#ifndef FLEETMANAGERSYSTEM_RESTAPI_HPP
#define FLEETMANAGERSYSTEM_RESTAPI_HPP
#pragma once

#include <boost/asio/awaitable.hpp>
#include <string>

#include "Database.hpp"
#include "http_server.hpp"

class RestApi {
public:
    RestApi(Database& database, ApiRouter& router);

private:
    asio::awaitable<HttpResponse> list_robots(const HttpRequest& req);
    asio::awaitable<HttpResponse> get_robot(const HttpRequest& req, std::string robot_id);
    asio::awaitable<HttpResponse> get_factsheet(const HttpRequest& req, std::string robot_id);

    Database& database_;
};


#endif //FLEETMANAGERSYSTEM_RESTAPI_HPP
