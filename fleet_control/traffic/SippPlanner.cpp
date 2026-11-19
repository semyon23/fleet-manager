#include "SippPlanner.hpp"
#include "TravelTime.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace traffic {

namespace {

constexpr auto kNoState = std::numeric_limits<std::size_t>::max();

// Самый ранний выезд в заданный безопасный интервал следующего узла.
std::optional<Time> earliest_departure(
    Time arrival, Interval source, Interval target, Time duration,
    const std::vector<Interval>& lanes, Time latest_arrival) {

    const Time earliest = std::max(
        arrival, target.begin > duration ? target.begin - duration : Time{0});

    for (const Interval& lane : lanes) {
        if (lane.end <= earliest) continue;
        const Time departure = std::max(earliest, lane.begin);

        // departure < source.end: узел занят до departure + 1.
        // Вычитание перед сложением защищает от переполнения.
        if (departure >= source.end || departure > latest_arrival ||
            duration > latest_arrival - departure) {
            return std::nullopt;
        }
        const Time next_arrival = departure + duration;
        if (next_arrival >= target.end) return std::nullopt;
        if (next_arrival <= lane.end) return departure;
    }
    return std::nullopt;
}

struct State {
    std::uint32_t node;
    Interval safe;
    Time arrival = kForever;
    std::size_t parent = kNoState;
    std::uint32_t edge = 0;
    Time departure = 0;  // выезд из parent
};

}  // namespace

struct SippPlanner::Impl {
    struct Arc {
        std::uint32_t to;
        std::uint32_t edge;
        ResourceId lane;
        Time duration;
    };

    ResourceMap resources;
    std::size_t node_count;
    std::vector<std::size_t> offsets;
    std::vector<Arc> arcs;

    explicit Impl(const Graph& graph)
        : resources(graph), node_count(graph.nodes.size()) {}
};

SippPlanner::SippPlanner(const Graph& graph, double default_speed)
    : impl_(std::make_unique<Impl>(graph)) {
    if (!std::isfinite(default_speed) || default_speed <= 0) {
        throw std::invalid_argument("SIPP default speed must be finite and positive");
    }
    if (graph.edges.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("too many SIPP edges");
    }
    Impl& s = *impl_;
    s.offsets.assign(s.node_count + 1, 0);
    for (const auto& edge : graph.edges) ++s.offsets[edge.from + 1];
    std::partial_sum(s.offsets.begin(), s.offsets.end(), s.offsets.begin());

    s.arcs.resize(graph.edges.size());
    auto next = s.offsets;
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        const auto& edge = graph.edges[i];
        const auto index = static_cast<std::uint32_t>(i);
        s.arcs[next[edge.from]++] = {
            edge.to, index, s.resources.lane_resource(index),
            travel_time(edge, default_speed)
        };
    }
}

SippPlanner::~SippPlanner() = default;
SippPlanner::SippPlanner(SippPlanner&&) noexcept = default;
SippPlanner& SippPlanner::operator=(SippPlanner&&) noexcept = default;

SippResult SippPlanner::search(
    const ReservationTable& table, const SippRequest& request) const {

    const Impl& s = *impl_;
    if (table.resources().map_id() != s.resources.map_id() ||
        table.resources().version() != s.resources.version() ||
        table.resources().size() != s.resources.size()) {
        throw std::invalid_argument("SIPP graph and reservation map differ");
    }
    if (request.start_time < 0 || request.latest_arrival < request.start_time ||
        request.latest_arrival == kForever || request.goals.empty() ||
        request.max_expansions == 0) {
        throw std::invalid_argument("invalid SIPP request");
    }
    if (request.from >= s.node_count) {
        throw std::out_of_range("SIPP start is outside graph");
    }
    for (auto goal : request.goals) {
        if (goal >= s.node_count) throw std::out_of_range("SIPP goal is outside graph");
    }

    const std::unordered_set<std::uint32_t> goals(
        request.goals.begin(), request.goals.end());
    std::vector<State> states;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> nodes;
    std::unordered_map<ResourceId, std::vector<Interval>> lanes;

    // Безопасные интервалы запрашиваем только для затронутых узлов.
    auto node_states = [&](std::uint32_t node) -> const std::vector<std::size_t>& {
        auto [entry, inserted] = nodes.try_emplace(node);
        if (inserted) {
            auto safe = table.safe_intervals(
                s.resources.node_resource(node),
                {request.start_time, kForever}, request.robot);
            for (const Interval& interval : safe) {
                if (interval.begin > request.latest_arrival) break;
                entry->second.push_back(states.size());
                states.push_back(State{node, interval});
            }
        }
        return entry->second;
    };

    using Item = std::pair<Time, std::size_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    SippResult result;

    for (std::size_t id : node_states(request.from)) {
        if (states[id].safe.begin <= request.start_time &&
            request.start_time < states[id].safe.end) {
            states[id].arrival = request.start_time;
            open.emplace(request.start_time, id);
            break;
        }
    }
    if (open.empty()) {
        result.status = SippStatus::StartBlocked;
        return result;
    }

    while (!open.empty()) {
        const auto [arrival, id] = open.top();
        open.pop();
        if (arrival != states[id].arrival) continue;
        if (result.expanded >= request.max_expansions) {
            result.status = SippStatus::ExpansionLimit;
            return result;
        }
        ++result.expanded;

        // Копия: добавление следующих состояний может переместить states.
        const State current = states[id];
        if (goals.contains(current.node) && current.safe.end == kForever) {
            result.status = SippStatus::Found;
            std::vector<std::size_t> path;
            for (auto at = id; at != kNoState; at = states[at].parent) path.push_back(at);
            std::reverse(path.begin(), path.end());

            for (std::size_t i = 0; i < path.size(); ++i) {
                const State& visit = states[path[i]];
                const bool last = i + 1 == path.size();
                const Time departure = last ? kForever : states[path[i + 1]].departure;
                result.visits.push_back({visit.node, visit.arrival, departure});
                result.reservations.push_back({
                    s.resources.node_resource(visit.node),
                    {visit.arrival, last ? kForever : departure + 1}
                });
                if (!last) {
                    const State& next_visit = states[path[i + 1]];
                    result.edges.push_back(next_visit.edge);
                    result.reservations.push_back({
                        s.resources.lane_resource(next_visit.edge),
                        {departure, next_visit.arrival}
                    });
                }
            }
            return result;
        }

        for (auto i = s.offsets[current.node]; i < s.offsets[current.node + 1]; ++i) {
            const auto& arc = s.arcs[i];
            if (arc.duration > request.latest_arrival - current.arrival) continue;

            auto [lane, inserted] = lanes.try_emplace(arc.lane);
            if (inserted) {
                lane->second = table.safe_intervals(
                    arc.lane, {request.start_time, kForever}, request.robot);
            }
            for (std::size_t target : node_states(arc.to)) {
                auto departure = earliest_departure(
                    current.arrival, current.safe, states[target].safe,
                    arc.duration, lane->second, request.latest_arrival);
                if (!departure) continue;
                const Time next_arrival = *departure + arc.duration;
                if (next_arrival >= states[target].arrival) continue;

                states[target].arrival = next_arrival;
                states[target].parent = id;
                states[target].edge = arc.edge;
                states[target].departure = *departure;
                open.emplace(next_arrival, target);
            }
        }
    }
    return result;
}

}  // namespace traffic
