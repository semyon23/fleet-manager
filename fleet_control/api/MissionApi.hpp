#ifndef FLEETMANAGERSYSTEM_MISSIONAPI_HPP
#define FLEETMANAGERSYSTEM_MISSIONAPI_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <string>
#include "http_server.hpp"
#include "maps/MapStore.hpp"
#include "missions/MissionStore.hpp"
#include "missions/MissionRunner.hpp"
#include "services/Services.hpp"

// Миссии (контракт UI, раздел 7.2): создание и отмена. Читает их UI из базы (GET /missions).
class MissionApi {
public:
    MissionApi(Services& services, MapStore& maps, MissionStore& missions, MissionRunner& runner, ApiRouter& router);

private:
    asio::awaitable<HttpResponse> create_mission(const HttpRequest& req);
    asio::awaitable<HttpResponse> cancel_mission(const HttpRequest& req, std::string mission_id);

    Services& services_;
    MapStore& maps_;
    MissionStore& missions_;
    MissionRunner& runner_;
};

#endif //FLEETMANAGERSYSTEM_MISSIONAPI_HPP
