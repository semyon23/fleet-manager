#ifndef FLEETMANAGERSYSTEM_RESERVATIONS_HPP
#define FLEETMANAGERSYSTEM_RESERVATIONS_HPP
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ResourceMap.hpp"

namespace traffic {

    using RobotId = std::uint32_t;

    // Почему набор ресурсов не удалось захватить.
    struct Conflict {
        ResourceId resource;
        RobotId owner;
    };

    // Жёсткие резервы исполнения для одной версии карты.
    //
    // Один поток: вызывать из потока control.
    // Истечения по времени нет. Освобождение — отдельный явный вызов.
    //
    // Повторный захват своим роботом успешен.
    // Это владение, а не счётчик ссылок: два захвата не требуют
    // двух освобождений.
    //
    // ResourceId брать из resources() этой таблицы.
    // RobotId должен обозначать одного робота и не переиспользоваться,
    // пока за ним остаются резервы.
    class Reservations {
    public:
        explicit Reservations(ResourceMap resources);

        const ResourceMap& resources() const { return resources_; }

        // nullopt — весь набор захвачен.
        // Conflict — ничего нового не захвачено,
        // прежние резервы всех роботов сохранены.
        //
        // Неверный ResourceId → out_of_range, без изменений таблицы.
        std::optional<Conflict> try_acquire(
            RobotId robot,
            std::span<const ResourceId> resources);

        // Освободить набор.
        // Свободный ресурс пропускается: повтор освобождения успешен.
        // Чужой ресурс → false, весь набор остаётся без изменений.
        //
        // Неверный ResourceId → out_of_range, без изменений таблицы.
        bool release(
            RobotId robot,
            std::span<const ResourceId> resources);

        std::optional<RobotId> owner(ResourceId resource) const;

    private:
        void check_resources(std::span<const ResourceId> resources) const;

        ResourceMap resources_;
        std::vector<std::optional<RobotId>> owners_;
    };

}  // namespace traffic

#endif //FLEETMANAGERSYSTEM_RESERVATIONS_HPP
