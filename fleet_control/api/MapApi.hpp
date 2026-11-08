#ifndef FLEETMANAGERSYSTEM_MAPAPI_HPP
#define FLEETMANAGERSYSTEM_MAPAPI_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <string>

#include "http_server.hpp"
#include "maps/MapStore.hpp"
#include "services/Services.hpp"

class MapApi {
public:
    MapApi(Services& services, MapStore& maps, ApiRouter& router);

private:
    asio::awaitable<HttpResponse> create_map(const HttpRequest& req);
    asio::awaitable<HttpResponse> update_map(const HttpRequest& req, std::string map_id);
    asio::awaitable<HttpResponse> delete_map(const HttpRequest& req, std::string map_id);
    asio::awaitable<HttpResponse> conflict_with_current(const HttpRequest& req, const std::string& map_id);
    asio::awaitable<std::pair<grpc::Status, std::vector<std::string>>> robots_losing_node(const std::string& map_id,const Graph& graph);
    Services& services_;
    MapStore& maps_;
};


#endif //FLEETMANAGERSYSTEM_MAPAPI_HPP
