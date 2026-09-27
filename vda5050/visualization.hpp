#pragma once

#include <cstdint>
#include <optional>

#include "common.hpp"
#include "types.hpp"

namespace vda5050 {

struct Visualization {
    Header header;
    std::uint32_t reference_state_header_id = 0;  // headerId state-сообщения, к которому относится
    std::optional<PlannedPath> planned_path;
    std::optional<IntermediatePath> intermediate_path;
    std::optional<MobileRobotPosition> mobile_robot_position;
    std::optional<Velocity> velocity;
};

inline void to_json(json& j, const Visualization& v) {
    j = json::object();
    put_header(j, v.header);
    detail::put(j, "referenceStateHeaderId", v.reference_state_header_id);
    detail::put(j, "plannedPath", v.planned_path);
    detail::put(j, "intermediatePath", v.intermediate_path);
    detail::put(j, "mobileRobotPosition", v.mobile_robot_position);
    detail::put(j, "velocity", v.velocity);
}

inline void from_json(const json& j, Visualization& v) {
    get_header(j, v.header);
    detail::get(j, "referenceStateHeaderId", v.reference_state_header_id);
    detail::get(j, "plannedPath", v.planned_path);
    detail::get(j, "intermediatePath", v.intermediate_path);
    detail::get(j, "mobileRobotPosition", v.mobile_robot_position);
    detail::get(j, "velocity", v.velocity);
}

}  // namespace vda5050
