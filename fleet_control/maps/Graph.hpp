#ifndef FLEETMANAGERSYSTEM_GRAPH_HPP
#define FLEETMANAGERSYSTEM_GRAPH_HPP

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Граф карты: узлы и рёбра по номерам. Номер узла — индекс в Graph::nodes, ребра — в Graph::edges.
// Номера действуют только внутри одной версии карты: после сохранения карты узел может получить
// другой номер. Поэтому наружу (dispatch, база, ответы API) уходят только строковые id.

struct GraphNode {
    std::string id;
    double x = 0;                                // м, координаты карты
    double y = 0;
    std::optional<double> theta;                 // рад; нет — любая ориентация
    bool station = false;
    bool has_edges = false;                      // есть входящие или исходящие рёбра
    std::vector<uint32_t> interaction_nodes;     // станция: узлы, с которых к ней подъезжают
};

struct GraphEdge {
    std::string id;
    uint32_t from = 0;                           // направленное ребро from → to;
    uint32_t to = 0;                             // двусторонняя дорога — два ребра
    double length = 0;                           // м, по прямой
    std::optional<double> max_speed;             // м/с
    double cost = 0;                             // доп. штраф, с (контракт 6.1); отрицательный — 0
};

struct Graph {
    std::string map_id;
    uint32_t version = 0;
    std::vector<GraphNode> nodes;                             // waypoints, потом stations
    std::vector<GraphEdge> edges;
    std::unordered_map<std::string, uint32_t> node_index;     // id → номер

    std::optional<uint32_t> find_node(const std::string& id) const;

    // Куда приехать, чтобы «быть на» узле: сам узел — waypoint или станция с рёбрами;
    // станция без рёбер — её узлы взаимодействия (поиск выберет ближайший)
    std::vector<uint32_t> arrival_nodes(uint32_t node) const;
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