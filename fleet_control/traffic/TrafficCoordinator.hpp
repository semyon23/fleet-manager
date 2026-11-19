#ifndef FLEETMANAGERSYSTEM_TRAFFICCOORDINATOR_HPP
#define FLEETMANAGERSYSTEM_TRAFFICCOORDINATOR_HPP
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "traffic/SippPlanner.hpp"

namespace traffic {

struct CoordinationRequest {
    RobotId robot = 0;
    std::uint32_t from = 0;
    std::vector<std::uint32_t> goals;
    std::uint32_t priority = 1;  // LOW=0, NORMAL=1, HIGH=2, CRITICAL=3
    Time waiting_since = 0;
    Time latest_arrival = 0;
};

struct CoordinationLimits {
    Time aging_period = 30'000; // +1 к приоритету за период ожидания
    std::size_t max_attempts = 8;
    std::size_t max_expansions = 100'000; // общий бюджет всех поисков
};

enum class CoordinationStatus {
    Accepted,
    StartConflict,
    NoPlanFound,
    BudgetExhausted
};

struct CoordinatedPlan {
    RobotId robot;
    SippResult path;
};

struct CoordinationResult {
    CoordinationStatus status = CoordinationStatus::NoPlanFound;
    std::vector<CoordinatedPlan> plans; // только при Accepted, в порядке поиска
    std::optional<RobotId> blocked_robot;
    std::optional<SippStatus> search_status;
    std::optional<TimeConflict> conflict;
    std::size_t attempts = 0;
    std::size_t expanded = 0;
};

// Автономное планирование одной группы на одной версии карты, один поток.
// Участники стоят в from в момент now и НЕ имеют выданного base.
// waiting_since сохраняется вызывающим между попытками.
// Все остальные роботы должны быть представлены резервами в table.
// Старые прогнозы участников заменяемы целиком; реальные hard-резервы
// этот модуль не читает и не изменяет. Для исполнения пока не подключать.
//
// Accepted заменяет планы всей группы; любой отказ/исключение сохраняет table.
// Набор участников задаёт вызывающий: автоматического поиска зависимостей нет.
// Поиск неполный: неудача не доказывает тупик и не означает FAILED миссии.
// Временная реализация копирует таблицу для изоляции попыток; это не
// оптимизированная реализация для 500 роботов. Бюджет не является тайм-аутом.
class TrafficCoordinator {
public:
    TrafficCoordinator(const Graph& graph, double default_speed);

    CoordinationResult replan(
        ReservationTable& table,
        Time now,
        std::span<const CoordinationRequest> requests,
        const CoordinationLimits& limits = {}) const;

private:
    ResourceMap resources_;
    std::size_t node_count_;
    SippPlanner planner_;
};

} // namespace traffic

#endif //FLEETMANAGERSYSTEM_TRAFFICCOORDINATOR_HPP
