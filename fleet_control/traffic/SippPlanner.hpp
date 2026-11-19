#ifndef FLEETMANAGERSYSTEM_SIPPPLANNER_HPP
#define FLEETMANAGERSYSTEM_SIPPPLANNER_HPP
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "maps/Graph.hpp"
#include "traffic/ReservationTable.hpp"

namespace traffic {

struct SippRequest {
    RobotId robot = 0;
    std::uint32_t from = 0;
    std::vector<std::uint32_t> goals;
    Time start_time = 0;
    Time latest_arrival = 0;
    std::size_t max_expansions = 100'000;
};

enum class SippStatus {
    Found,
    StartBlocked,
    NoPathWithinWindow,
    ExpansionLimit
};

struct SippVisit {
    std::uint32_t node;
    Time arrival;
    Time departure;  // kForever у конечного узла
};

struct SippResult {
    SippStatus status = SippStatus::NoPathWithinWindow;
    std::vector<SippVisit> visits;
    std::vector<std::uint32_t> edges;
    std::vector<TimedReservation> reservations;
    std::size_t expanded = 0;
};

// SIPP для одного робота, по одному снимку графа и таблицы.
// Ожидание разрешено на любом узле. Оптимизируем время прибытия.
// cost не является временем движения и здесь не используется.
//
// Граф копируется в компактные массивы при создании планировщика.
// На каждый поиск создаются данные только посещённых узлов/ресурсов.
// Первая версия использует нулевую эвристику: очередь по времени прибытия.
//
// Поиск таблицу не изменяет. Принять результат — отдельный try_replace().
// Во время поиска таблицу менять нельзя.
// from/goals — физические узлы Graph, для станции вызывающий использует arrival_nodes().
class SippPlanner {
public:
    SippPlanner(const Graph& graph, double default_speed);
    ~SippPlanner();
    SippPlanner(SippPlanner&&) noexcept;
    SippPlanner& operator=(SippPlanner&&) noexcept;

    // Требуется 0 <= start_time <= latest_arrival < kForever,
    // непустые goals и положительный max_expansions.
    // Ошибки параметров/версии карты -> invalid_argument/out_of_range.
    SippResult search(const ReservationTable& table, const SippRequest& request) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace traffic

#endif //FLEETMANAGERSYSTEM_SIPPPLANNER_HPP
