#include "FixedRouteGate.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace traffic {

GateResult FixedRouteGate::check(
    const Graph& graph,
    std::span<const TrafficOccupant> occupants,
    std::span<const ProtectedRoute> routes,
    const ProtectedRoute& wanted) const {

    BaseGuard guard(graph);
    std::map<std::string, RobotId> ids;
    std::map<std::string, const ProtectedRoute*> by_robot;

    auto id = [&](const std::string& robot) {
        if (robot.empty()) {
            throw std::invalid_argument("empty traffic robot");
        }

        auto found = ids.find(robot);
        if (found != ids.end()) {
            return found->second;
        }

        if (ids.size() >= std::numeric_limits<RobotId>::max()) {
            throw std::length_error("too many traffic robots");
        }

        return ids.emplace(
            robot, static_cast<RobotId>(ids.size())).first->second;
    };

    auto validate = [&](const ProtectedRoute& route) {
        if (route.robot.empty() ||
            route.key.empty() ||
            route.map != graph.map_id ||
            route.version != graph.version ||
            route.nodes.empty() ||
            route.edges.size() != route.nodes.size() - 1 ||
            route.reached >= route.nodes.size()) {
            throw std::invalid_argument("invalid protected route");
        }

        for (auto node : route.nodes) {
            if (node >= graph.nodes.size()) {
                throw std::out_of_range("traffic node");
            }
        }

        for (std::size_t i = 0; i < route.edges.size(); ++i) {
            const auto& edge = graph.edges.at(route.edges[i]);

            if (edge.from != route.nodes[i] ||
                edge.to != route.nodes[i + 1]) {
                throw std::invalid_argument(
                    "traffic edge topology mismatch");
            }
        }
    };

    validate(wanted);

    // Сначала восстанавливаем все уже выданные разрешения.
    for (const auto& route : routes) {
        if (route.map != graph.map_id) continue;

        validate(route);

        if (!by_robot.emplace(route.robot, &route).second) {
            throw std::invalid_argument(
                "multiple protected routes for one robot");
        }

        const auto robot = id(route.robot);
        const auto nodes =
            std::span(route.nodes).subspan(route.reached);
        const auto edges =
            std::span(route.edges).subspan(route.reached);

        if (guard.begin(robot, 1, nodes.front()) ||
            guard.extend(robot, 1, nodes, edges)) {
            return {
                false,
                "existing protected routes conflict"
            };
        }
    }

    // Затем учитываем стоящих роботов, включая роботов без миссии.
    std::set<std::string> seen;

    for (const auto& occupant : occupants) {
        if (!seen.insert(occupant.robot).second) {
            throw std::invalid_argument(
                "duplicate traffic occupant");
        }

        if (occupant.map.empty()) {
            return {false, "robot map is unknown"};
        }

        if (occupant.map != graph.map_id) {
            if (by_robot.contains(occupant.robot)) {
                return {false, "robot changed map"};
            }

            continue;
        }

        if (by_robot.contains(occupant.robot)) continue;

        if (!occupant.stationary ||
            occupant.has_mission ||
            occupant.node.empty()) {
            return {
                false,
                "untracked robot position or base is unknown"
            };
        }

        const auto node = graph.node_index.find(occupant.node);
        if (node == graph.node_index.end()) {
            return {false, "robot is outside graph"};
        }

        if (guard.begin(id(occupant.robot), 1, node->second)) {
            return {
                false,
                "stationary robot conflicts with protected route"
            };
        }
    }

    for (const auto& [robot, route] : by_robot) {
        if (!seen.contains(robot)) {
            return {
                false,
                "protected robot missing from inventory"
            };
        }
    }

    auto previous = by_robot.find(wanted.robot);

    if (previous == by_robot.end() ||
        !seen.contains(wanted.robot)) {
        return {
            false,
            "target robot has no protected origin"
        };
    }

    const auto& old = *previous->second;

    if (old.key != wanted.key ||
        old.reached != wanted.reached ||
        old.nodes.size() > wanted.nodes.size() ||
        !std::equal(
            old.nodes.begin(), old.nodes.end(),
            wanted.nodes.begin()) ||
        !std::equal(
            old.edges.begin(), old.edges.end(),
            wanted.edges.begin())) {
        throw std::invalid_argument(
            "protected prefix changed");
    }

    const auto nodes =
        std::span(wanted.nodes).subspan(wanted.reached);
    const auto edges =
        std::span(wanted.edges).subspan(wanted.reached);

    if (guard.extend(id(wanted.robot), 1, nodes, edges)) {
        return {false, "requested route is occupied"};
    }

    return {true, {}};
}

} // namespace traffic