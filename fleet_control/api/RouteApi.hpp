#ifndef FLEETMANAGERSYSTEM_ROUTEAPI_HPP
#define FLEETMANAGERSYSTEM_ROUTEAPI_HPP

#pragma once
#include <boost/asio/awaitable.hpp>
#include "http_server.hpp"
#include "missions/MissionRunner.hpp"

// GET /api/fms/routes?mapId= (c2): маршруты роботов для Live Map. Их знает только control (в памяти),
// поэтому этот GET шлюз отдаёт control, а не базе
class RouteApi {
public:
    RouteApi(MissionRunner& runner, ApiRouter& router);

private:
    asio::awaitable<HttpResponse> list(const HttpRequest& req);
    MissionRunner& runner_;
};

#endif //FLEETMANAGERSYSTEM_ROUTEAPI_HPP