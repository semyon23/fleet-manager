
#ifndef VDA_TOPICS_HPP
#define VDA_TOPICS_HPP
#pragma once



#include <string>
#include <vector>
#include "include/robot/robot_config.hpp"

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
    std::string get_order_topic() const { return  order;};
    std::string get_instant_actions_topic() const { return  instant_actions;};
    std::vector<std::string> subscriptions() const { return {order, instant_actions}; }
};

#endif //VDA_TOPICS_HPP
