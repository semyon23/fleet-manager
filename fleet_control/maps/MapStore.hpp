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

class MapStore {
public:
    MapStore(asio::io_context& io_context, Services& services);

    void start();
    void stop();


    bool loaded() const { return loaded_; }

    const Graph* find(const std::string& map_id) const;
    void put(Graph graph);
    void remove(const std::string& map_id);

private:
    asio::awaitable<void> load();
    void add_record(const fms::database::v1::MapRecord& map);

    asio::io_context& io_context_;
    Services& services_;
    std::unordered_map<std::string, Graph> graphs_;
    bool loaded_{false};
    bool stopped_{false};
    asio::steady_timer retry_timer_;
};



#endif //FLEETMANAGERSYSTEM_MAPSTORE_HPP
