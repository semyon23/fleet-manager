
#ifndef VDA_TOPICS_HPP
#define VDA_TOPICS_HPP
#pragma once

// Имена всех MQTT-топиков робота (VDA 5050 v3.0.0, 4.2, 4.3).
// Схема: <interface>/<major_version>/<manufacturer>/<serial_number>/<topic>
// Строится один раз из IdentityConfig: StatePublisher берёт исходящие,
// транспорт через subscriptions() — входящие. Так они не могут разойтись.

#include <string>
#include <vector>

#include "robot_config.hpp"

struct VdaTopics {
    std::string state;
    std::string visualization;
    std::string connection;
    std::string factsheet;

    std::string order;
    std::string instant_actions;
    std::string zone_set;
    std::string responses;

    explicit VdaTopics(const IdentityConfig& id) {
        const std::string p = id.topic_prefix();
        state           = p + "state";
        visualization   = p + "visualization";
        connection      = p + "connection";
        factsheet       = p + "factsheet";
        order           = p + "order";
        instant_actions = p + "instantActions";
        zone_set        = p + "zoneSet";
        responses       = p + "responses";
    }

    std::vector<std::string> subscriptions() const { return {order, instant_actions}; }
};

#endif //VDA_TOPICS_HPP
