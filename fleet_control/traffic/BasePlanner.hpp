#ifndef FLEETMANAGERSYSTEM_BASEPLANNER_HPP
#define FLEETMANAGERSYSTEM_BASEPLANNER_HPP

#pragma once

#include "traffic/BaseGuard.hpp"
#include "traffic/SippPlanner.hpp"

namespace traffic {

struct BasePlanRequest {
    RobotId robot = 0;
    std::uint64_t epoch = 0;
    Time now = 0;
    Time latest_arrival = 0;
    std::vector<std::uint32_t> goals;
    std::size_t max_expansions = 100'000;
};

enum class BasePlanStatus {
    Extended,
    Waiting,
    BoundaryIsGoal,
    BaseOutsideWindow,
    SearchFailed,
    ForecastConflict,
    HardConflict
};

struct BasePlanResult {
    BasePlanStatus status = BasePlanStatus::SearchFailed;
    Time boundary_arrival = 0;
    SippResult tail;
    std::optional<TimeConflict> forecast_conflict;
    std::optional<Conflict> hard_conflict;

    // Полный base для отправки, заполнен ТОЛЬКО при Extended.
    std::vector<std::uint32_t> nodes;
    std::vector<std::uint32_t> edges;
};

// Один поток, одна версия карты и общая шкала времени ReservationTable.
// Один робот относительно сохранённых прогнозов остальных; не замена
// группового TrafficCoordinator. Все роботы должны быть зарегистрированы
// в общем BaseGuard, их прогнозы — в table. Не создавать отдельный guard
// для каждого робота. Вызовы now должны следовать монотонному времени.
//
// Extended: ресурсы уже захвачены, далее интеграция отправляет команду.
// Waiting/BoundaryIsGoal: обновлён только прогноз, base не изменён.
// Остальные статусы: table и guard сохранены. Исключения тоже не меняют их.
// Ошибка пересчёта НЕ подтверждает актуальность старого прогноза:
// интеграция должна обработать конфликт/задержку до дальнейшей выдачи.
// Ни один статус не завершает миссию. BoundaryIsGoal означает, что граница base
// является целью, а не что робот уже прибыл или закончил действия.
// После неясного результата отправки нельзя вызывать заново как новую
// команду: сначала требуется согласование с dispatch (следующий этап).
class BasePlanner {
public:
    BasePlanner(const Graph& graph, double default_speed);

    BasePlanResult update(
        BaseGuard& guard,
        ReservationTable& table,
        const BasePlanRequest& request) const;

private:
    SippPlanner planner_;
    ResourceMap resources_;
    std::size_t node_count_;
    std::vector<Time> durations_;
};

} // namespace traffic


#endif //FLEETMANAGERSYSTEM_BASEPLANNER_HPP
