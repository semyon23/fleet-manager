#ifndef FLEETMANAGERSYSTEM_RESOURCEMAP_HPP
#define FLEETMANAGERSYSTEM_RESOURCEMAP_HPP
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "maps/Graph.hpp"

namespace traffic {

    using ResourceId = std::uint32_t;

    // Ресурсы одной версии Graph.
    //
    // Каждый узел получает отдельный ресурс.
    // Все рёбра A→B и B→A, включая параллельные,
    // получают один общий ресурс проезда.
    //
    // Номера узлов и рёбер — из Graph, переданного конструктору.
    // После изменения карты нужно построить новую ResourceMap.
    // Старые резервы нельзя переносить на новые индексы.
    //
    // Ссылку на Graph не хранит.
    class ResourceMap {
    public:
        explicit ResourceMap(const Graph& graph);

        ResourceId node_resource(std::uint32_t node) const;
        ResourceId lane_resource(std::uint32_t edge) const;

        std::size_t size() const { return resource_count_; }

        const std::string& map_id() const { return map_id_; }
        std::uint32_t version() const { return version_; }

    private:
        std::string map_id_;
        std::uint32_t version_ = 0;
        std::size_t node_count_ = 0;
        std::size_t resource_count_ = 0;

        // Индекс ребра Graph → общий ресурс проезда.
        std::vector<ResourceId> edge_lanes_;
    };

}  // namespace traffic

#endif //FLEETMANAGERSYSTEM_RESOURCEMAP_HPP
