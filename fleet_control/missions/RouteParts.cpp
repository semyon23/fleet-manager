#include "RouteParts.hpp"

std::optional<RouteParts> split_route(std::span<const std::string> nodes,std::span<const std::string> edges, const fms::v1::MoveProgress& progress) {

    if (nodes.empty() || edges.size() != nodes.size() - 1) {
        return std::nullopt;
    }

    const size_t from = progress.last_node_index();
    const size_t released = progress.released_nodes();

    if (progress.total_nodes() != nodes.size() ||
        released == 0 ||
        released > nodes.size() ||
        from >= released) {
        return std::nullopt;
        }

    if (!progress.last_node_id().empty() &&
        progress.last_node_id() != nodes[from]) {
        return std::nullopt;
        }

    RouteParts result;

    result.released.nodes.assign(
        nodes.begin() + from,
        nodes.begin() + released);

    result.released.edges.assign(
        edges.begin() + from,
        edges.begin() + released - 1);

    if (released < nodes.size()) {
        // Например:
        // маршрут  a -- b -- c -- d
        // released a -- b
        // planned       b -- c -- d
        result.planned.nodes.assign(
            nodes.begin() + released - 1,
            nodes.end());

        result.planned.edges.assign(
            edges.begin() + released - 1,
            edges.end());
    }

    return result;
}