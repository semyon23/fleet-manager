#include "ResourceMap.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace traffic {

ResourceMap::ResourceMap(const Graph& graph)
    : map_id_(graph.map_id),
      version_(graph.version),
      node_count_(graph.nodes.size()),
      resource_count_(graph.nodes.size()) {

    constexpr auto max_resources =
        std::numeric_limits<ResourceId>::max();

    if (node_count_ > max_resources) {
        throw std::length_error("too many traffic resources");
    }

    edge_lanes_.reserve(graph.edges.size());

    // Неупорядоченная пара узлов → ресурс проезда.
    // A→B и B→A имеют одинаковый ключ.
    std::map<std::pair<std::uint32_t, std::uint32_t>, ResourceId> lanes;

    for (const GraphEdge& edge : graph.edges) {
        if (edge.from >= node_count_ || edge.to >= node_count_) {
            throw std::invalid_argument(
                "traffic edge endpoint is outside graph");
        }

        // build_graph() уже исключает такие рёбра.
        if (edge.from == edge.to) {
            throw std::invalid_argument(
                "traffic graph must not contain self-loops");
        }

        const auto key = std::make_pair(
            std::min(edge.from, edge.to),
            std::max(edge.from, edge.to));

        auto found = lanes.find(key);
        if (found != lanes.end()) {
            edge_lanes_.push_back(found->second);
            continue;
        }

        if (resource_count_ >= max_resources) {
            throw std::length_error("too many traffic resources");
        }

        const auto resource =
            static_cast<ResourceId>(resource_count_);

        lanes.emplace(key, resource);
        edge_lanes_.push_back(resource);
        ++resource_count_;
    }
}

ResourceId ResourceMap::node_resource(std::uint32_t node) const {
    if (node >= node_count_) {
        throw std::out_of_range("traffic node index is outside graph");
    }

    return node;
}

ResourceId ResourceMap::lane_resource(std::uint32_t edge) const {
    return edge_lanes_.at(edge);
}

}  // namespace traffic