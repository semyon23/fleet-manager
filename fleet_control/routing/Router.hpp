#ifndef FLEETMANAGERSYSTEM_ROUTER_HPP
#define FLEETMANAGERSYSTEM_ROUTER_HPP


#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "maps/Graph.hpp"

namespace routing {

// Путь по графу одной карты. Номера — как в Graph, которым построен Router
struct Route {
    std::vector<uint32_t> nodes;   // nodes[0] — старт, nodes.back() — выбранная цель
    std::vector<uint32_t> edges;   // edges[i] ведёт из nodes[i] в nodes[i+1]
    double seconds = 0;
};

// Поиск пути по графу карты: «навигатор на пустом складе», без других роботов.
// Вес ребра — время проезда: длина / скорость + cost; скорость — maxSpeed ребра или default_speed.
// Ребро с maxSpeed 0 — закрыто. Один поток: буферы поиска общие, Router не потокобезопасен.
// Номера узлов должны быть из того же Graph, что и в конструкторе.
class Router {
public:
    Router(const Graph& graph, double default_speed);
    ~Router();
    Router(Router&&) noexcept;
    Router& operator=(Router&&) noexcept;

    // Быстрейший путь от from до ближайшей по времени из targets. Нет пути — nullopt.
    // from среди targets — путь из одного узла, 0 с
    std::optional<Route> route(uint32_t from, std::span<const uint32_t> targets);

    // Время от каждого sources[i] до ближайшей из targets, с. Одна обратная волна от targets.
    // Останавливается, когда найдены max_found ближайших или волна ушла дальше max_seconds;
    // не найденные — бесконечность
    std::vector<double> times_to(std::span<const uint32_t> targets, std::span<const uint32_t> sources,
                                 size_t max_found, double max_seconds);

    // Сильно связные компоненты: внутри одной из любого узла можно доехать до любого.
    // У нормального склада компонента одна; больше — есть ловушки (проверки карты, шаг 5)
    uint32_t component_count() const;
    uint32_t component(uint32_t node) const;

private:
    struct Impl;                    // граф BGL, время рёбер, буферы поиска
    std::unique_ptr<Impl> impl_;
};

}  // namespace routing

#endif //FLEETMANAGERSYSTEM_ROUTER_HPP


