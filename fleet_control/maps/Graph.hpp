#ifndef FLEETMANAGERSYSTEM_GRAPH_HPP
#define FLEETMANAGERSYSTEM_GRAPH_HPP

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct GraphNode {
    std::string id;
    double x = 0;
    double y = 0;
    std::optional<double> theta;                 // рад; нет — любая ориентация
    bool station = false;
    std::vector<std::string> interaction_nodes;  // станция: узлы, с которых к ней подъезжают
    std::vector<size_t> out;                     // индексы рёбер Graph::edges, выходящих из узла
};

struct GraphEdge {
    std::string id;
    std::string from;                            // направленное ребро from → to;
    std::string to;                              // двусторонняя дорога — два ребра
    double length = 0;                           // м, по прямой
    std::optional<double> max_speed;             // м/с
    double cost = 0;                             // доп. штраф, с (контракт 6.1); отрицательный — 0
};

struct Graph {
    std::string map_id;
    uint32_t version = 0;
    std::unordered_map<std::string, GraphNode> nodes;   // waypoints и stations
    std::vector<GraphEdge> edges;
};

struct MapCheck {
    std::vector<std::string> errors;     // карту не сохранить: по ней не построить маршрут
    std::vector<std::string> warnings;   // сохранить можно
};

// Документ редактора: width, height, meta, waypoints, edges, stations, ...
MapCheck validate_map(const nlohmann::json& body);

// Только для документа, в котором validate_map не нашёл ошибок
Graph build_graph(const std::string& map_id, uint32_t version, const nlohmann::json& body);


#endif //FLEETMANAGERSYSTEM_GRAPH_HPP
