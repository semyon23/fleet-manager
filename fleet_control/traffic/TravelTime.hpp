#ifndef FLEETMANAGERSYSTEM_TRAVELTIME_HPP
#define FLEETMANAGERSYSTEM_TRAVELTIME_HPP

#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "maps/Graph.hpp"
#include "traffic/ReservationTable.hpp"

namespace traffic {

    inline Time travel_time(const GraphEdge& edge, double default_speed) {
        const double speed = edge.max_speed.value_or(default_speed);

        if (!std::isfinite(edge.length) || edge.length < 0 ||
            !std::isfinite(speed) || speed <= 0) {
            throw std::invalid_argument("invalid edge length or speed");
            }

        const long double ms = std::ceil(
            static_cast<long double>(edge.length) / speed * 1000);

        if (!std::isfinite(ms) || ms >= static_cast<long double>(kForever)) {
            throw std::invalid_argument("edge duration is too large");
        }

        return std::max<Time>(1, static_cast<Time>(ms));
    }

} // namespace traffic


#endif //FLEETMANAGERSYSTEM_TRAVELTIME_HPP
