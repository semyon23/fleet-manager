#include "Reservations.hpp"

#include <stdexcept>
#include <utility>

namespace traffic {

    Reservations::Reservations(ResourceMap resources)
        : resources_(std::move(resources)),
          owners_(resources_.size()) {}

    void Reservations::check_resources(std::span<const ResourceId> resources) const {

        for (ResourceId resource : resources) {
            if (resource >= owners_.size()) {
                throw std::out_of_range(
                    "traffic resource index is outside map");
            }
        }
    }

    std::optional<Conflict> Reservations::try_acquire(
        RobotId robot,
        std::span<const ResourceId> resources) {

        check_resources(resources);

        // Сначала проверяем весь набор.
        // Ни одного изменения до завершения проверки.
        for (ResourceId resource : resources) {
            const auto& held = owners_[resource];

            if (held && *held != robot) {
                return Conflict{resource, *held};
            }
        }

        // Всё свободно или уже принадлежит этому роботу.
        for (ResourceId resource : resources) {
            owners_[resource] = robot;
        }

        return std::nullopt;
    }

    bool Reservations::release(
        RobotId robot,
        std::span<const ResourceId> resources) {

        check_resources(resources);

        // Чужое владение нельзя снять даже частично.
        for (ResourceId resource : resources) {
            const auto& held = owners_[resource];

            if (held && *held != robot) {
                return false;
            }
        }

        for (ResourceId resource : resources) {
            owners_[resource].reset();
        }

        return true;
    }

    std::optional<RobotId> Reservations::owner(
        ResourceId resource) const {

        return owners_.at(resource);
    }

}  // namespace traffic