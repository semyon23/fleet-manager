#pragma once
#ifndef ROBOTAPIHANDLER_HPP
#define ROBOTAPIHANDLER_HPP

#include "include/api/BaseApiHandler.hpp"
#include "include/core/RobotsService.hpp"
#include "include/core/FleetDispatchClient.hpp"



class RobotApiHandler : BaseApiHandler{
public:
    RobotApiHandler(std::shared_ptr<fms::RobotsService> robotsServ,
                     std::shared_ptr<FleetDispatchClient> dispatchClient
                    );

    asio::awaitable<http::message_generator> onCreateRobot(http::request<http::string_body>&& req);
    asio::awaitable<http::message_generator> onDeleteRobot(http::request<http::string_body>&& req);
    asio::awaitable<http::message_generator> getAllRobots(http::request<http::string_body>&& req);
private:
    std::shared_ptr<fms::RobotsService> robots_service;
    std::shared_ptr<FleetDispatchClient> dispatch_client;
};



#endif //ROBOTAPIHANDLER_HPP
