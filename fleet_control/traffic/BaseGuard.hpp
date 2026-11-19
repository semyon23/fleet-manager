#ifndef FLEETMANAGERSYSTEM_BASEGUARD_HPP
#define FLEETMANAGERSYSTEM_BASEGUARD_HPP

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "maps/Graph.hpp"
#include "traffic/Reservations.hpp"

namespace traffic {

struct BaseSnapshot {
    std::uint64_t epoch;
    std::vector<std::uint32_t> nodes;
    std::vector<std::uint32_t> edges;
    std::size_t reached;
};

// Одна версия карты, один поток. Роботы-точки.
// Владеет жёсткими резервами, не прогнозами SIPP.
// Вызывать extend ДО отправки команды роботу. При потерянном ответе
// резервы сохраняются: отката по тайм-ауту здесь намеренно нет.
// nodes/edges — выданный или уже зарезервированный для отправки base,
// без horizon. Старый префикс менять нельзя.
// epoch — монотонное поколение маршрута робота, не revision ExtendMission.
// Привязку epoch к mission/step и проверку достоверности прогресса делает
// интеграция. После перезапуска требуется восстановление до выдачи команд.
class BaseGuard {
public:
    explicit BaseGuard(const Graph& graph);

    // Зарегистрировать стоящего робота. Новый epoch существующего робота
    // разрешён лишь после достижения конца base и на том же конечном узле.
    // Повтор того же epoch/start идемпотентен, прогресс не сбрасывает.
    std::optional<Conflict> begin(
        RobotId robot, std::uint64_t epoch, std::uint32_t node);

    // Полный base текущего поколения: nodes.size() == edges.size() + 1.
    // Конфликт сохраняет старый base и все резервы без изменений.
    std::optional<Conflict> extend(
        RobotId robot, std::uint64_t epoch,
        std::span<const std::uint32_t> nodes,
        std::span<const std::uint32_t> edges);

    // Только подтверждённое достижение узла, НЕ ETA и НЕ ближайшая координата.
    // Чужой epoch или откат индекса -> false, ничего не меняется.
    // Индекс за концом base -> out_of_range. Повтор индекса -> true.
    bool advance(RobotId robot, std::uint64_t epoch, std::size_t reached);

    BaseSnapshot snapshot(RobotId robot) const;
    std::optional<RobotId> owner(ResourceId resource) const;
    const ResourceMap& resources() const { return reservations_.resources(); }

private:
    struct State {
        BaseSnapshot base;
        std::vector<ResourceId> held;
    };
    std::vector<ResourceId> remaining(const BaseSnapshot& base) const;

    Reservations reservations_;
    std::size_t node_count_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> endpoints_;
    std::unordered_map<RobotId, State> robots_;
};

} // namespace traffic


#endif //FLEETMANAGERSYSTEM_BASEGUARD_HPP
