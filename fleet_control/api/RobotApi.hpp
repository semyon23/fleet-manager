#ifndef FLEETMANAGERSYSTEM_ROBOTAPI_HPP
#define FLEETMANAGERSYSTEM_ROBOTAPI_HPP
#pragma once

#include <boost/asio/awaitable.hpp>
#include <string>

#include "services/Services.hpp"
#include "fleet/FleetSync.hpp"
#include "http_server.hpp"


class RobotApi {
public:
    RobotApi(Services& services, FleetSync& sync, AvailabilityTracker& availability, MapStore& maps, ApiRouter& router);

private:
    asio::awaitable<HttpResponse> create_robot(const HttpRequest& req);
    asio::awaitable<HttpResponse> update_robot(const HttpRequest& req, std::string robot_id);

    asio::awaitable<HttpResponse> delete_robot(const HttpRequest& req, std::string robot_id);
    asio::awaitable<HttpResponse> pause_robot(const HttpRequest& req, std::string robot_id, bool pause);
    asio::awaitable<HttpResponse> set_position(const HttpRequest& req, std::string robot_id);

    Services& services_;
    FleetSync& sync_;
    AvailabilityTracker& availability_;
    MapStore& maps_;
};


#endif //FLEETMANAGERSYSTEM_ROBOTAPI_HPP
