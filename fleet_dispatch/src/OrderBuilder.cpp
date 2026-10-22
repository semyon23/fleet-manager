#include "../include/OrderBuilder.hpp"
#include "vda5050.hpp"
#include <google/protobuf/util/json_util.h>

namespace {

std::string validate_actions(const google::protobuf::RepeatedPtrField<fms::v1::Action>& actions) {
    for (const auto& action : actions) {
        if (action.action_type().empty()) {
            return "action_type is required";
        }
        if (action.blocking_type() == fms::v1::BLOCKING_TYPE_UNSPECIFIED) {
            return "blocking_type is required for action " + action.action_type();
        }
    }
    return {};
}


vda5050::BlockingType to_vda(fms::v1::BlockingType type) {
    switch (type) {
        case fms::v1::BLOCKING_TYPE_SOFT:   return vda5050::BlockingType::Soft;
        case fms::v1::BLOCKING_TYPE_SINGLE: return vda5050::BlockingType::Single;
        case fms::v1::BLOCKING_TYPE_HARD:   return vda5050::BlockingType::Hard;
        default:                            return vda5050::BlockingType::None;
    }
}

// google.protobuf.Value (любой JSON) → nlohmann::json
nlohmann::json to_json_value(const google::protobuf::Value& value) {
    std::string text;
    (void)google::protobuf::util::MessageToJsonString(value, &text);
    return nlohmann::json::parse(text);
}

// action_id: если control не задал — генерируем из orderId и места действия, например "m1-s1-n2-0"
vda5050::Action to_vda(const fms::v1::Action& action, const std::string& generated_id) {
    vda5050::Action out;
    out.action_type = action.action_type();
    out.action_id = action.action_id().empty() ? generated_id : action.action_id();
    out.blocking_type = to_vda(action.blocking_type());
    if (!action.descriptor_().empty()) {
        out.action_descriptor = action.descriptor_();
    }
    if (!action.parameters().empty()) {
        out.action_parameters.emplace();
        for (const auto& parameter : action.parameters()) {
            out.action_parameters->push_back({parameter.key(), to_json_value(parameter.value())});
        }
    }
    if (action.retriable()) {
        out.retriable = true;
    }
    return out;
}

std::vector<vda5050::Action> to_vda(const google::protobuf::RepeatedPtrField<fms::v1::Action>& actions,
                                    const std::string& id_prefix) {
    std::vector<vda5050::Action> out;
    for (int i = 0; i < actions.size(); ++i) {
        out.push_back(to_vda(actions[i], id_prefix + "-" + std::to_string(i)));
    }
    return out;
}

vda5050::Node to_vda(const fms::v1::RouteNode& node, std::uint32_t sequence_id, bool released,
                     const std::string& action_id_prefix) {
    vda5050::NodePosition position;
    position.x = node.x();
    position.y = node.y();
    position.map_id = node.map_id();
    if (node.has_theta()) {
        position.theta = node.theta();
    }
    if (node.has_allowed_deviation_xy()) {
        // в proto одно число — круг; в VDA эллипс, задаём a = b
        double radius = node.allowed_deviation_xy();
        position.allowed_deviation_xy = vda5050::AllowedDeviationXY{radius, radius, 0.0};
    }
    if (node.has_allowed_deviation_theta()) {
        position.allowed_deviation_theta = node.allowed_deviation_theta();
    }

    vda5050::Node out;
    out.node_id = node.node_id();
    out.sequence_id = sequence_id;
    out.released = released;
    out.node_position = position;
    out.actions = to_vda(node.actions(), action_id_prefix);
    return out;
}

vda5050::Edge to_vda(const fms::v1::RouteEdge& edge, std::uint32_t sequence_id, bool released, const std::string& action_id_prefix) {
    vda5050::Edge out;
    out.edge_id = edge.edge_id();
    out.sequence_id = sequence_id;
    out.released = released;
    if (edge.has_maximum_speed()) {
        out.maximum_speed = edge.maximum_speed();
    }
    if (edge.has_orientation()) {
        out.orientation = edge.orientation();
        out.orientation_type = edge.orientation_type() == fms::v1::ORIENTATION_TYPE_GLOBAL
                                   ? vda5050::OrientationType::Global
                                   : vda5050::OrientationType::Tangential;
    }
    out.actions = to_vda(edge.actions(), action_id_prefix);
    return out;
}

}  // namespace

std::string validate_move_step(const fms::v1::MoveStep& step) {
    if (step.nodes().empty()) {
        return "move: nodes are empty";
    }
    if (step.edges_size() != step.nodes_size() - 1) {
        return "move: edges must be nodes - 1";
    }
    if (step.has_released_nodes() &&
        (step.released_nodes() < 1 || step.released_nodes() > static_cast<std::uint32_t>(step.nodes_size()))) {
        return "move: released_nodes must be from 1 to the number of nodes";
    }
    for (const auto& node : step.nodes()) {
        if (node.node_id().empty() || node.map_id().empty()) {
            return "move: node_id and map_id are required";
        }
        if (auto error = validate_actions(node.actions()); !error.empty()) {
            return "move: node " + node.node_id() + ": " + error;
        }
    }
    for (const auto& edge : step.edges()) {
        if (edge.edge_id().empty()) {
            return "move: edge_id is required";
        }
        if (auto error = validate_actions(edge.actions()); !error.empty()) {
            return "move: edge " + edge.edge_id() + ": " + error;
        }
    }
    return {};
}

std::string validate_action_step(const fms::v1::ActionStep& step) {
    if (step.actions().empty()) {
        return "action: actions are empty";
    }
    return validate_actions(step.actions());
}

vda5050::Order build_move_order(const std::string& order_id, const fms::v1::MoveStep& step) {
    // Сколько узлов в base. Не задано — весь маршрут.
    int released_count = step.has_released_nodes() ? static_cast<int>(step.released_nodes()) : step.nodes_size();

    vda5050::Order order;
    order.order_id = order_id;
    order.order_update_id = 0;

    for (int i = 0; i < step.nodes_size(); ++i) {
        // узлы: sequenceId 0, 2, 4, ...
        order.nodes.push_back(to_vda(step.nodes(i), 2 * i, i < released_count,
                                     order_id + "-n" + std::to_string(i)));
    }
    for (int i = 0; i < step.edges_size(); ++i) {
        // ребро i между узлами i и i+1: sequenceId 1, 3, 5, ...; released, если released его конечный узел
        order.edges.push_back(to_vda(step.edges(i), 2 * i + 1, i + 1 < released_count,
                                     order_id + "-e" + std::to_string(i)));
    }
    return order;
}

vda5050::Order build_action_order(const std::string& order_id, const fms::v1::ActionStep& step,
                                  const fms::v1::RobotStatus& robot) {
    // Узел, где робот стоит: его id и текущая поза
    fms::v1::RouteNode here;
    here.set_node_id(robot.last_node_id());
    here.set_x(robot.pose().x());
    here.set_y(robot.pose().y());
    here.set_map_id(robot.pose().map_id());
    *here.mutable_actions() = step.actions();

    vda5050::Order order;
    order.order_id = order_id;
    order.order_update_id = 0;
    order.nodes.push_back(to_vda(here, 0, true, order_id + "-n0"));
    return order;
}