#include "BasePlanner.hpp"
#include "TravelTime.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace traffic {

BasePlanner::BasePlanner(const Graph& graph, double default_speed)
    : planner_(graph, default_speed),
      resources_(graph),
      node_count_(graph.nodes.size()) {

    durations_.reserve(graph.edges.size());
    for (const auto& edge : graph.edges) {
        durations_.push_back(travel_time(edge, default_speed));
    }
}

BasePlanResult BasePlanner::update(
    BaseGuard& guard,
    ReservationTable& table,
    const BasePlanRequest& r) const {

    auto matches = [&](const ResourceMap& map) {
        return map.map_id() == resources_.map_id() &&
               map.version() == resources_.version() &&
               map.size() == resources_.size();
    };

    if (!matches(guard.resources()) || !matches(table.resources())) {
        throw std::invalid_argument("base planning map mismatch");
    }

    if (r.now < 0 ||
        r.latest_arrival < r.now ||
        r.latest_arrival == kForever ||
        r.goals.empty() ||
        r.max_expansions == 0) {
        throw std::invalid_argument("invalid base planning request");
    }

    for (auto goal : r.goals) {
        if (goal >= node_count_) {
            throw std::out_of_range("goal outside graph");
        }
    }

    const auto base = guard.snapshot(r.robot);
    if (base.epoch != r.epoch) {
        throw std::invalid_argument("stale base epoch");
    }

    BasePlanResult result;
    Time time = r.now;
    std::vector<TimedReservation> forecast;

    // Пройденное не включаем. Если робот между узлами, считаем полное
    // следующее ребро от последнего подтверждённого узла.
    for (auto i = base.reached; i < base.edges.size(); ++i) {
        const auto duration = durations_.at(base.edges[i]);

        if (duration > r.latest_arrival - time) {
            result.status = BasePlanStatus::BaseOutsideWindow;
            return result;
        }

        forecast.push_back({
            resources_.node_resource(base.nodes[i]),
            {time, time + 1}
        });
        forecast.push_back({
            resources_.lane_resource(base.edges[i]),
            {time, time + duration}
        });

        time += duration;
    }

    result.boundary_arrival = time;

    SippRequest search;
    search.robot = r.robot;
    search.from = base.nodes.back();
    search.goals = r.goals;
    search.start_time = time;
    search.latest_arrival = r.latest_arrival;
    search.max_expansions = r.max_expansions;

    result.tail = planner_.search(table, search);
    if (result.tail.status != SippStatus::Found) {
        return result;
    }

    forecast.insert(
        forecast.end(),
        result.tail.reservations.begin(),
        result.tail.reservations.end());

    // Переписываем прогноз целиком, но вместе с оставшимся выданным base.
    // Копия обеспечивает откат и при конфликте hard-ресурсов ниже.
    ReservationTable candidate = table;

    if (auto conflict = candidate.try_replace(r.robot, forecast)) {
        result.status = BasePlanStatus::ForecastConflict;
        result.forecast_conflict = conflict;
        return result;
    }

    auto nodes = base.nodes;
    auto edges = base.edges;

    for (std::size_t i = 0; i < result.tail.edges.size(); ++i) {
        const auto& visit = result.tail.visits[i];

        // Нельзя выдать участок за будущим ожиданием: робот не обязан
        // соблюдать времена SIPP. До узла ожидания ехать разрешаем.
        if (visit.departure > visit.arrival &&
            visit.departure > r.now) {
            break;
        }

        edges.push_back(result.tail.edges[i]);
        nodes.push_back(result.tail.visits[i + 1].node);
    }

    if (edges.size() == base.edges.size()) {
        result.status = result.tail.edges.empty() ? BasePlanStatus::BoundaryIsGoal : BasePlanStatus::Waiting;
    } else {
        // Подготовить результат до изменения guard: после захвата
        // не должно остаться выделений памяти или бросающих операций.
        result.nodes = std::move(nodes);
        result.edges = std::move(edges);

        if (auto conflict = guard.extend(
                r.robot, r.epoch, result.nodes, result.edges)) {
            result.nodes.clear();
            result.edges.clear();
            result.status = BasePlanStatus::HardConflict;
            result.hard_conflict = conflict;
            return result;
        }

        result.status = BasePlanStatus::Extended;
    }

    static_assert(
        std::is_nothrow_move_assignable_v<ReservationTable>);
    static_assert(
        std::is_nothrow_move_constructible_v<BasePlanResult>);

    table = std::move(candidate);
    return result;
}

} // namespace traffic