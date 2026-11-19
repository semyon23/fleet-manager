#include "BaseGuard.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <type_traits>

namespace traffic {

BaseGuard::BaseGuard(const Graph& graph)
    : reservations_(ResourceMap{graph}), node_count_(graph.nodes.size()) {
    endpoints_.reserve(graph.edges.size());
    for (const auto& edge : graph.edges) {
        endpoints_.emplace_back(edge.from, edge.to);
    }
}

std::vector<ResourceId> BaseGuard::remaining(const BaseSnapshot& base) const {
    std::vector<ResourceId> result;
    for (auto i = base.reached; i < base.nodes.size(); ++i) {
        result.push_back(resources().node_resource(base.nodes[i]));
    }
    for (auto i = base.reached; i < base.edges.size(); ++i) {
        result.push_back(resources().lane_resource(base.edges[i]));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::optional<Conflict> BaseGuard::begin(
    RobotId robot, std::uint64_t epoch, std::uint32_t node) {

    if (node >= node_count_) {
        throw std::out_of_range("base start outside graph");
    }

    auto old = robots_.find(robot);
    if (old != robots_.end()) {
        const auto& base = old->second.base;
        if (epoch == base.epoch && node == base.nodes.front()) {
            return std::nullopt;
        }
        if (epoch <= base.epoch ||
            base.reached != base.nodes.size() - 1 ||
            node != base.nodes.back()) {
            throw std::invalid_argument("cannot replace outstanding base");
        }
    }

    State next{
        {epoch, {node}, {}, 0},
        {resources().node_resource(node)}
    };

    // Все выделения памяти до захвата; фиксация State не бросает.
    static_assert(std::is_nothrow_move_assignable_v<State>);

    auto [entry, inserted] = robots_.try_emplace(robot);
    if (auto conflict = reservations_.try_acquire(robot, next.held)) {
        if (inserted) robots_.erase(entry);
        return conflict;
    }

    entry->second = std::move(next);
    return std::nullopt;
}

std::optional<Conflict> BaseGuard::extend(
    RobotId robot, std::uint64_t epoch,
    std::span<const std::uint32_t> nodes,
    std::span<const std::uint32_t> edges) {

    auto& state = robots_.at(robot);
    const auto& old = state.base;

    if (epoch != old.epoch ||
        nodes.empty() ||
        edges.size() != nodes.size() - 1 ||
        nodes.size() < old.nodes.size() ||
        edges.size() < old.edges.size()) {
        throw std::invalid_argument("invalid base extension");
    }

    if (!std::equal(old.nodes.begin(), old.nodes.end(), nodes.begin()) ||
        !std::equal(old.edges.begin(), old.edges.end(), edges.begin())) {
        throw std::invalid_argument("base prefix is immutable");
    }

    for (auto node : nodes) {
        if (node >= node_count_) {
            throw std::out_of_range("base node outside graph");
        }
    }

    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (edges[i] >= endpoints_.size()) {
            throw std::out_of_range("base edge outside graph");
        }
        if (endpoints_[edges[i]] != std::pair{nodes[i], nodes[i + 1]}) {
            throw std::invalid_argument("base edge does not connect nodes");
        }
    }

    State next{
        {
            epoch,
            {nodes.begin(), nodes.end()},
            {edges.begin(), edges.end()},
            old.reached
        },
        {}
    };
    next.held = remaining(next.base);

    if (auto conflict = reservations_.try_acquire(robot, next.held)) {
        return conflict;
    }

    state = std::move(next);
    return std::nullopt;
}

bool BaseGuard::advance(RobotId robot, std::uint64_t epoch, std::size_t reached) {

    auto& state = robots_.at(robot);
    if (epoch != state.base.epoch || reached < state.base.reached) {
        return false;
    }
    if (reached >= state.base.nodes.size()) {
        throw std::out_of_range("progress exceeds base");
    }
    if (reached == state.base.reached) return true;

    auto next = state.base;
    next.reached = reached;
    auto held = remaining(next);

    std::vector<ResourceId> released;
    std::set_difference(
        state.held.begin(), state.held.end(),
        held.begin(), held.end(),
        std::back_inserter(released));

    if (!reservations_.release(robot, released)) {
        throw std::logic_error("base ownership invariant violated");
    }

    state.base.reached = reached;
    state.held.swap(held);
    return true;
}

BaseSnapshot BaseGuard::snapshot(RobotId robot) const {
    return robots_.at(robot).base;
}

std::optional<RobotId> BaseGuard::owner(ResourceId resource) const {
    return reservations_.owner(resource);
}

} // namespace traffic