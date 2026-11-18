#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "ResourceMap.hpp"
#include "Types.hpp"

namespace traffic {

// Миллисекунды относительно общего начала планирования.
// Это не Unix time. Все планы одной таблицы используют одно начало.
using Time = std::int64_t;

inline constexpr Time kForever = std::numeric_limits<Time>::max();

struct Interval {
    Time begin;
    Time end;

    bool operator==(const Interval&) const = default;
};

struct TimedReservation {
    ResourceId resource;
    Interval interval;
};

struct TimeConflict {
    ResourceId resource;
    RobotId owner;

    // Пересечение запрошенного и существующего интервалов.
    Interval interval;
};

// Временные резервы планировщика для одной версии карты.
//
// Один поток. Ссылку на исходный Graph не хранит.
// Жёсткие резервы исполнения не изменяет.
//
// Интервалы [begin, end):
//   begin >= 0, begin < end;
//   end == kForever — окончание пока неизвестно.
//
// ResourceId брать из resources() этой таблицы.
// RobotId не переиспользовать, пока у робота остаётся план.
class ReservationTable {
public:
    explicit ReservationTable(ResourceMap resources);

    const ResourceMap& resources() const { return resources_; }

    // Заменить весь временной план робота.
    //
    // nullopt — новый план принят.
    // TimeConflict — старый план и вся таблица сохранены.
    //
    // Перекрывающиеся и соседние интервалы одного робота
    // на одном ресурсе объединяются.
    //
    // Пустой план удаляет временные резервы этого робота.
    // Его реальные жёсткие резервы при этом остаются.
    //
    // Неверный ресурс → out_of_range.
    // Неверный интервал → invalid_argument.
    // При этих ошибках таблица не меняется.
    std::optional<TimeConflict> try_replace(
        RobotId robot,
        std::span<const TimedReservation> plan);

    // Свободные интервалы внутри window.
    //
    // При перепланировании можно исключить прежний план
    // самого робота через ignore_robot.
    //
    // Неверный ресурс → out_of_range.
    // Неверный window → invalid_argument.
    std::vector<Interval> safe_intervals(
        ResourceId resource,
        Interval window,
        std::optional<RobotId> ignore_robot = std::nullopt) const;

private:
    struct Booking {
        RobotId robot;
        Interval interval;
    };

    ResourceMap resources_;

    // Индекс ресурса → отсортированные резервы.
    std::vector<std::vector<Booking>> bookings_;

    // Какие ресурсы затрагивает план робота.
    // Для замены не нужно обходить всю карту.
    std::unordered_map<RobotId, std::vector<ResourceId>> robot_resources_;
};

}  // namespace traffic