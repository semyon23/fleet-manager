#pragma once

#include <vector>

#include "common.hpp"
#include "types.hpp"

namespace vda5050 {

struct InstantActions {
    Header header;
    std::vector<Action> actions;
};

inline void to_json(json& j, const InstantActions& v) {
    j = json::object();
    put_header(j, v.header);
    detail::put(j, "actions", v.actions);
}

inline void from_json(const json& j, InstantActions& v) {
    get_header(j, v.header);
    detail::get(j, "actions", v.actions);
}

}  // namespace vda5050
