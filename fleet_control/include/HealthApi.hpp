#ifndef FLEETMANAGERSYSTEM_HEALTHAPI_HPP
#define FLEETMANAGERSYSTEM_HEALTHAPI_HPP

#pragma once
#include <boost/asio/awaitable.hpp>
#include "Services.hpp"
#include "http_server.hpp"

class HealthApi {
public:
    HealthApi(Services& services, ApiRouter& router);

private:
    asio::awaitable<HttpResponse> health(const HttpRequest& req);
    Services& services_;
};


#endif //FLEETMANAGERSYSTEM_HEALTHAPI_HPP
