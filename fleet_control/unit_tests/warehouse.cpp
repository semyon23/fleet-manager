#include "warehouse.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using nlohmann::json;

json generate_warehouse(const WarehouseOptions& options) {
    constexpr double kResolution = 0.05;
    constexpr double kMargin = 5.0;

    json waypoints = json::array();
    json stations = json::array();
    json edges = json::array();

    auto pixels = [&](double meters) { return std::round(meters / kResolution); };
    auto node_id = [](int aisle, int i) { return "a" + std::to_string(aisle) + "_" + std::to_string(i); };
    auto add_edge = [&](const std::string& from, const std::string& to) {
        edges.push_back({{"id", "e" + std::to_string(edges.size())}, {"from", from}, {"to", to}});
    };

    for (int aisle = 0; aisle < options.aisles; ++aisle) {
        for (int i = 0; i < options.nodes_per_aisle; ++i) {
            waypoints.push_back({{"id", node_id(aisle, i)},
                                 {"u", pixels(kMargin + i * options.step)},
                                 {"v", pixels(kMargin + aisle * options.aisle_gap)}});
        }

        // Вдоль прохода: двусторонний — обе стороны; иначе чётный — вправо, нечётный — влево
        const bool two_way = aisle % options.two_way_every == 0;
        const bool rightward = aisle % 2 == 0;
        for (int i = 1; i < options.nodes_per_aisle; ++i) {
            if (two_way || rightward) {
                add_edge(node_id(aisle, i - 1), node_id(aisle, i));
            }
            if (two_way || !rightward) {
                add_edge(node_id(aisle, i), node_id(aisle, i - 1));
            }
        }

        // Поперечные проходы к следующему проходу. В начале и в конце — обязательно:
        // без них из одностороннего прохода не развернуться, и граф развалится на части
        if (aisle + 1 < options.aisles) {
            for (int i = 0; i < options.nodes_per_aisle; ++i) {
                if (i % options.cross_every == 0 || i == options.nodes_per_aisle - 1) {
                    add_edge(node_id(aisle, i), node_id(aisle + 1, i));
                    add_edge(node_id(aisle + 1, i), node_id(aisle, i));
                }
            }
        }
    }

    // Станции слева, равномерно по проходам, у начала прохода
    const int station_count = std::min(options.stations, options.aisles);
    for (int s = 0; s < station_count; ++s) {
        const int aisle = s * options.aisles / station_count;
        const std::string id = "st" + std::to_string(s);
        stations.push_back({{"id", id},
                            {"u", pixels(kMargin - 3)},
                            {"v", pixels(kMargin + aisle * options.aisle_gap)},
                            {"kind", "charge"},
                            {"interactionNodeIds", {node_id(aisle, 0)}}});
        add_edge(id, node_id(aisle, 0));
        add_edge(node_id(aisle, 0), id);
    }

    const double width = 2 * kMargin + (options.nodes_per_aisle - 1) * options.step;
    const double height = 2 * kMargin + (options.aisles - 1) * options.aisle_gap;
    return {{"name", "Synthetic warehouse " + std::to_string(options.aisles * options.nodes_per_aisle)},
            {"width", pixels(width)},
            {"height", pixels(height)},
            {"meta", {{"resolution", kResolution}, {"origin", {0, 0, 0}}}},
            {"waypoints", waypoints},
            {"stations", stations},
            {"edges", edges},
            {"zones", json::array()}};
}