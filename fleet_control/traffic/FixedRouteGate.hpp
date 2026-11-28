#ifndef FLEETMANAGERSYSTEM_FIXEDROUTEGATE_HPP
#define FLEETMANAGERSYSTEM_FIXEDROUTEGATE_HPP

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "traffic/BaseGuard.hpp"

namespace traffic {

    // Входы должны описывать ВСЕХ зарегистрированных роботов.
    // Робот без известной карты блокирует выдачу на всех картах.
    struct TrafficOccupant {
        std::string robot;
        std::string map;
        std::string node;
        bool stationary = false;
        bool has_mission = false;
    };

    // Только фактические/уже зарезервированные разрешения, без horizon.
    struct ProtectedRoute {
        std::string robot;
        std::string key;
        std::string map;
        std::uint32_t version = 0;
        std::vector<std::uint32_t> nodes;
        std::vector<std::uint32_t> edges;
        std::size_t reached = 0;
    };

    struct GateResult {
        bool granted = false;
        std::string reason;
        // Абсолютное число узлов base, включая уже пройденные.
        std::size_t released_nodes = 0;
    };

    // All possibly issued step bases from one verified mission/attempt.
    // An empty list means the bounds of this mission are unknown.
    struct RecoveryMission {
        std::vector<ProtectedRoute> steps;
    };

    // Snapshot for one map. Set complete only after accounting for every
    // unresolved owner in the fleet, including its map and current attempt.
    // An unknown owner/map or contradictory order leaves complete false.
    struct RecoveryBounds {
        std::string map;
        std::uint32_t version = 0;
        bool complete = false;
        std::vector<RecoveryMission> missions;
    };
    // Один поток. Нет освобождения по ETA и нет геометрии роботов.
    // Реестр защищённых маршрутов хранится вызывающим кодом.
    //
    // Проверка строит временный BaseGuard из всех оставшихся разрешений.
    // Стоимость нужно измерить и оптимизировать перед большой нагрузкой.
    class FixedRouteGate {
    public:
        using CanWait = std::function<bool(std::uint32_t)>;

        GateResult check(
            const Graph& graph,
            std::span<const TrafficOccupant> occupants,
            std::span<const ProtectedRoute> protected_routes,
            const ProtectedRoute& wanted) const;

        GateResult check(
            const Graph &graph,
            std::span<const TrafficOccupant> occupants,
            std::span<const ProtectedRoute> protected_routes,
            const ProtectedRoute &wanted,
            const RecoveryBounds &recovery) const;

        // Возвращает границу для штатного продления. При конфликте разрешает
        // подъезд до последнего свободного узла, где допустимо ждать.
        GateResult plan_extension(
            const Graph& graph,
            std::span<const TrafficOccupant> occupants,
            std::span<const ProtectedRoute> protected_routes,
            const ProtectedRoute& wanted,
            const CanWait& can_wait) const;

    private:
        GateResult check_impl(
            const Graph& graph,
            std::span<const TrafficOccupant> occupants,
            std::span<const ProtectedRoute> protected_routes,
            const ProtectedRoute& wanted,
            const RecoveryBounds& recovery,
            const CanWait* can_wait) const;
    };

} // namespace traffic

#endif //FLEETMANAGERSYSTEM_FIXEDROUTEGATE_HPP
