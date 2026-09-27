#pragma once

// VDA 5050 v3.0.0 — топик `instantActions` (6.2.1, 7.4).
// Публикует: fleet control. Подписан: робот. QoS 0.
//
// Action выполняются сразу, вне order. blockingType у instant action всегда NONE.
// Статус каждой приходит обратно в state.instantActionStates;
// очищать список — action clearInstantActions.

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
