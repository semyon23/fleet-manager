#ifndef FLEETMANAGERSYSTEM_ROBOTAPI_HPP
#define FLEETMANAGERSYSTEM_ROBOTAPI_HPP
#pragma once

#include <boost/asio/awaitable.hpp>
#include <string>

#include "Services.hpp"
#include "http_server.hpp"


class RobotApi {
public:
    RobotApi(Services& services, ApiRouter& router);

private:
    asio::awaitable<HttpResponse> create_robot(const HttpRequest& req);
    asio::awaitable<HttpResponse> update_robot(const HttpRequest& req, std::string robot_id);
    asio::awaitable<void> add_to_dispatch(const fms::database::v1::RobotSpec& spec);

    asio::awaitable<HttpResponse> delete_robot(const HttpRequest& req, std::string robot_id);
    asio::awaitable<HttpResponse> pause_robot(const HttpRequest& req, std::string robot_id, bool pause);
    asio::awaitable<void> finish_delete(const std::string& robot_id);

    Services& services_;
};


#endif //FLEETMANAGERSYSTEM_ROBOTAPI_HPP
