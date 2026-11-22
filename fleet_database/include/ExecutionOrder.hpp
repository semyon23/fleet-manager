#ifndef FLEETMANAGERSYSTEM_EXECUTIONORDER_HPP
#define FLEETMANAGERSYSTEM_EXECUTIONORDER_HPP

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include "fms_types.pb.h"

namespace execution_order {

    // В update узлы начинаются с точки стыка. Их sequenceId остаются абсолютными.
    inline bool matches_route(const nlohmann::json& doc, const fms::v1::MissionNode& step,
                              std::uint32_t released, std::uint64_t update) {
        const auto& nodes = doc.at("nodes");
        const auto& edges = doc.at("edges");
        const std::size_t total = step.has_move() ? step.move().nodes_size() : 1;

        if (!nodes.is_array() || nodes.empty() || nodes.size() > total ||
            !edges.is_array() || edges.size() + 1 != nodes.size() ||
            released == 0 || released > total) return false;

        if (step.has_move() && step.move().edges_size() + 1 != step.move().nodes_size())
            return false;

        if (!step.has_move() && (!step.has_action() || update != 0)) return false;

        const auto first = total - nodes.size();
        if ((update == 0 && first != 0) || first >= released) return false;

        for (std::size_t i = first; i < total; ++i) {
            const auto& node = nodes.at(i - first);
            if (node.at("sequenceId") != 2 * i || node.at("released") != (i < released))
                return false;

            if (step.has_move() && node.at("nodeId") != step.move().nodes(i).node_id())
                return false;

            if (i + 1 < total) {
                const auto& edge = edges.at(i - first);
                if (edge.at("edgeId") != step.move().edges(i).edge_id() ||
                    edge.at("sequenceId") != 2 * i + 1 ||
                    edge.at("released") != (i + 1 < released))
                    return false;
            }
        }
        return true;
    }

}  // namespace execution_order

#endif //FLEETMANAGERSYSTEM_EXECUTIONORDER_HPP
