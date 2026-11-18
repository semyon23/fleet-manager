#include "ReservationTable.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace traffic {

namespace {

void check_interval(const Interval& interval) {
    if (interval.begin < 0 || interval.begin >= interval.end) {
        throw std::invalid_argument(
            "traffic interval must satisfy 0 <= begin < end");
    }
}

bool interval_less(const Interval& a, const Interval& b) {
    if (a.begin != b.begin) {
        return a.begin < b.begin;
    }
    return a.end < b.end;
}

// Интервалы одного робота на одном ресурсе.
// Пример: [2, 5), [4, 7), [7, 9) → [2, 9).
void normalize(std::vector<Interval>& intervals) {
    std::sort(intervals.begin(), intervals.end(), interval_less);

    std::vector<Interval> merged;
    merged.reserve(intervals.size());

    for (const Interval& interval : intervals) {
        if (merged.empty() || merged.back().end < interval.begin) {
            merged.push_back(interval);
        } else {
            merged.back().end =
                std::max(merged.back().end, interval.end);
        }
    }

    intervals = std::move(merged);
}

}  // namespace

ReservationTable::ReservationTable(ResourceMap resources)
    : resources_(std::move(resources)),
      bookings_(resources_.size()) {}

std::optional<TimeConflict> ReservationTable::try_replace(
    RobotId robot,
    std::span<const TimedReservation> plan) {

    // Сначала проверяем весь запрос и группируем его по ресурсам.
    // Таблицу пока не меняем.
    std::map<ResourceId, std::vector<Interval>> wanted;

    for (const TimedReservation& reservation : plan) {
        if (reservation.resource >= bookings_.size()) {
            throw std::out_of_range(
                "traffic resource index is outside map");
        }

        check_interval(reservation.interval);

        wanted[reservation.resource].push_back(
            reservation.interval);
    }

    for (auto& [resource, intervals] : wanted) {
        normalize(intervals);
    }

    // Проверяем новые интервалы против других роботов.
    // Старый план самого робота не мешает его замене.
    for (const auto& [resource, intervals] : wanted) {
        std::size_t current = 0;

        for (const Booking& held : bookings_[resource]) {
            if (held.robot == robot) {
                continue;
            }

            while (current < intervals.size() &&
                   intervals[current].end <= held.interval.begin) {
                ++current;
            }

            if (current == intervals.size()) {
                break;
            }

            const Interval& requested = intervals[current];

            if (requested.begin < held.interval.end) {
                return TimeConflict{
                    resource,
                    held.robot,
                    {
                        std::max(requested.begin, held.interval.begin),
                        std::min(requested.end, held.interval.end)
                    }
                };
            }
        }
    }

    // Новая версия только затронутых ресурсов:
    // старые ресурсы робота плюс ресурсы нового плана.
    std::map<ResourceId, std::vector<Booking>> replacements;

    if (auto old = robot_resources_.find(robot);
        old != robot_resources_.end()) {
        for (ResourceId resource : old->second) {
            replacements.try_emplace(resource);
        }
    }

    for (const auto& [resource, intervals] : wanted) {
        replacements.try_emplace(resource);
    }

    for (auto& [resource, next] : replacements) {
        auto added = wanted.find(resource);
        const std::size_t new_count =
            added == wanted.end() ? 0 : added->second.size();

        next.reserve(bookings_[resource].size() + new_count);

        // Убираем прежние интервалы только этого робота.
        for (const Booking& held : bookings_[resource]) {
            if (held.robot != robot) {
                next.push_back(held);
            }
        }

        if (added != wanted.end()) {
            for (const Interval& interval : added->second) {
                next.push_back(Booking{robot, interval});
            }
        }

        std::sort(
            next.begin(),
            next.end(),
            [](const Booking& a, const Booking& b) {
                return interval_less(a.interval, b.interval);
            });
    }

    std::vector<ResourceId> new_resources;
    new_resources.reserve(wanted.size());

    for (const auto& [resource, intervals] : wanted) {
        new_resources.push_back(resource);
    }

    // Всё подготовлено. Дальше только замена готовых данных.
    // Возможные выделения памяти завершены до изменения резервов.
    if (new_resources.empty()) {
        robot_resources_.erase(robot);
    } else {
        auto [entry, inserted] = robot_resources_.try_emplace(robot);
        entry->second.swap(new_resources);
    }

    for (auto& [resource, next] : replacements) {
        bookings_[resource].swap(next);
    }

    return std::nullopt;
}

std::vector<Interval> ReservationTable::safe_intervals(
    ResourceId resource,
    Interval window,
    std::optional<RobotId> ignore_robot) const {

    if (resource >= bookings_.size()) {
        throw std::out_of_range(
            "traffic resource index is outside map");
    }

    check_interval(window);

    std::vector<Interval> result;
    Time cursor = window.begin;

    for (const Booking& held : bookings_[resource]) {
        if (ignore_robot && held.robot == *ignore_robot) {
            continue;
        }

        if (held.interval.end <= cursor) {
            continue;
        }

        if (held.interval.begin >= window.end) {
            break;
        }

        if (held.interval.begin > cursor) {
            result.push_back(Interval{
                cursor,
                held.interval.begin
            });
        }

        cursor = std::max(cursor, held.interval.end);

        if (cursor >= window.end) {
            break;
        }
    }

    if (cursor < window.end) {
        result.push_back(Interval{cursor, window.end});
    }

    return result;
}

}  // namespace traffic