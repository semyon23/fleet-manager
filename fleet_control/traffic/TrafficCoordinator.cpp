#include "TrafficCoordinator.hpp"
#include <algorithm>
#include <numeric>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace traffic {

TrafficCoordinator::TrafficCoordinator(const Graph& graph, double default_speed)
    : resources_(graph), node_count_(graph.nodes.size()),
      planner_(graph, default_speed) {}

CoordinationResult TrafficCoordinator::replan(
    ReservationTable& table, Time now,
    std::span<const CoordinationRequest> requests,
    const CoordinationLimits& limits) const {

    if (now < 0 || now == kForever || limits.aging_period <= 0 ||
        limits.max_attempts == 0 || limits.max_expansions == 0) {
        throw std::invalid_argument("invalid coordination time or limits");
    }
    if (table.resources().map_id() != resources_.map_id() ||
        table.resources().version() != resources_.version() ||
        table.resources().size() != resources_.size()) {
        throw std::invalid_argument("coordinator and reservation map differ");
    }
    std::unordered_set<RobotId> robots;
    for (const auto& r : requests) {
        if (!robots.insert(r.robot).second || r.priority > 3 ||
            r.waiting_since < 0 || r.waiting_since > now ||
            r.latest_arrival < now || r.latest_arrival == kForever ||
            r.goals.empty()) {
            throw std::invalid_argument("invalid coordination request");
        }
        if (r.from >= node_count_) {
            throw std::out_of_range("coordination start outside graph");
        }
        for (auto goal : r.goals) {
            if (goal >= node_count_) {
                throw std::out_of_range("coordination goal outside graph");
            }
        }
    }

    CoordinationResult result;
    if (requests.empty()) {
        result.status = CoordinationStatus::Accepted;
        return result;
    }

    // Не удаляем участников из модели мира: вместо старых прогнозов
    // они сначала бессрочно стоят в своих текущих узлах.
    ReservationTable baseline = table;
    for (const auto& r : requests) {
        (void)baseline.try_replace(r.robot, {});
    }
    for (const auto& r : requests) {
        const TimedReservation hold{
            resources_.node_resource(r.from), {now, kForever}};
        if (auto conflict = baseline.try_replace(
                r.robot, std::span<const TimedReservation>(&hold, 1))) {
            result.status = CoordinationStatus::StartConflict;
            result.blocked_robot = r.robot;
            result.conflict = conflict;
            return result;
        }
    }

    std::vector<std::size_t> order(requests.size());
    std::iota(order.begin(), order.end(), 0);
    auto score = [&](const CoordinationRequest& r) {
        return static_cast<std::uint64_t>(r.priority) +
            static_cast<std::uint64_t>((now - r.waiting_since) / limits.aging_period);
    };
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        const auto& x = requests[a];
        const auto& y = requests[b];
        if (score(x) != score(y)) return score(x) > score(y);
        if (x.waiting_since != y.waiting_since) return x.waiting_since < y.waiting_since;
        return x.robot < y.robot;
    });

    std::set<std::vector<std::size_t>> tried;
    for (;;) {
        if (tried.contains(order)) return result;
        if (result.attempts == limits.max_attempts ||
            result.expanded == limits.max_expansions) {
            result.status = CoordinationStatus::BudgetExhausted;
            return result;
        }
        tried.insert(order);
        ++result.attempts;
        ReservationTable candidate = baseline;
        std::vector<CoordinatedPlan> plans;
        std::optional<std::size_t> failed;
        for (std::size_t position = 0; position < order.size(); ++position) {
            if (result.expanded == limits.max_expansions) {
                result.status = CoordinationStatus::BudgetExhausted;
                return result;
            }
            const auto& r = requests[order[position]];
            SippRequest search;
            search.robot = r.robot;
            search.from = r.from;
            search.goals = r.goals;
            search.start_time = now;
            search.latest_arrival = r.latest_arrival;
            search.max_expansions = limits.max_expansions - result.expanded;
            auto path = planner_.search(candidate, search);
            result.expanded += path.expanded;
            if (path.status != SippStatus::Found) {
                result.blocked_robot = r.robot;
                result.search_status = path.status;
                if (path.status == SippStatus::ExpansionLimit) {
                    result.status = CoordinationStatus::BudgetExhausted;
                    return result;
                }
                failed = position;
                break;
            }
            if (candidate.try_replace(r.robot, path.reservations)) {
                throw std::logic_error("SIPP produced conflicting reservations");
            }
            plans.push_back({r.robot, std::move(path)});
        }
        if (!failed) {
            // Единственная запись в исходную таблицу. После неё нет
            // выделений памяти или потенциально бросающих операций.
            static_assert(std::is_nothrow_move_assignable_v<ReservationTable>);
            result.plans = std::move(plans);
            result.status = CoordinationStatus::Accepted;
            result.blocked_robot.reset();
            result.search_status.reset();
            table = std::move(candidate);
            return result;
        }
        // Поздний участник мог быть заблокирован более ранним планом.
        // Пробуем его первым. Если первым не прошёл уже он — пробуем
        // циклический сдвиг, чтобы другой робот освободил стартовый узел.
        if (*failed == 0) {
            std::rotate(order.begin(), order.begin() + 1, order.end());
        } else {
            std::rotate(order.begin(), order.begin() + *failed,
                        order.begin() + *failed + 1);
        }
    }
}

} // namespace traffic