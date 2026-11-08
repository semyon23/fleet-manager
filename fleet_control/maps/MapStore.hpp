#ifndef FLEETMANAGERSYSTEM_MAPSTORE_HPP
#define FLEETMANAGERSYSTEM_MAPSTORE_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <string>
#include <unordered_map>

#include "maps/Graph.hpp"
#include "services/Services.hpp"
#include "routing/Router.hpp"

class MapStore {
public:
    MapStore(asio::io_context& io_context, Services& services, double default_speed);

    void start();
    void stop();


    bool loaded() const { return loaded_; }
    std::function<void()> on_change;
    const Graph* find(const std::string& map_id) const;
    routing::Router* router(const std::string& map_id);   // поиск пути по карте; nullptr — карты нет
    void remove(const std::string& map_id);

    void put(Graph graph);                            // строит Router сам — при загрузке карт
    void put(Graph graph, routing::Router router);   // Router уже построен — при сохранении карты
    double default_speed() const { return default_speed_; }
private:
    asio::awaitable<void> load();
    void add_record(const fms::database::v1::MapRecord& map);

    asio::io_context& io_context_;
    Services& services_;
   // std::unordered_map<std::string, Graph> graphs_;
    bool loaded_{false};
    bool stopped_{false};
    asio::steady_timer retry_timer_;
    struct Map {
        Graph graph;
        routing::Router router;
    };
    std::unordered_map<std::string, Map> maps_;
    double default_speed_;
};



#endif //FLEETMANAGERSYSTEM_MAPSTORE_HPP
