#include "Graph.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <set>

using nlohmann::json;

namespace {

const json* field(const json& object, const char* key) {
    auto it = object.find(key);
    return it == object.end() || it->is_null() ? nullptr : &*it;
}

// Список документа; нет или не массив — пустой (что это ошибка, говорит validate_map)
const json& list(const json& body, const char* key) {
    static const json empty = json::array();
    const json* value = field(body, key);
    return value && value->is_array() ? *value : empty;
}

bool positive(const json* value) {
    return value && value->is_number() && value->get<double>() > 0;
}

// Строковый id элемента списка; нет — nullptr
const std::string* id_of(const json& item) {
    const json* id = item.is_object() ? field(item, "id") : nullptr;
    return id && id->is_string() ? &id->get_ref<const std::string&>() : nullptr;
}

}  // namespace

MapCheck validate_map(const json& body) {
    MapCheck check;
    auto& errors = check.errors;
    auto& warnings = check.warnings;
    if (!body.is_object()) {
        errors.push_back("map must be a JSON object");
        return check;
    }

    for (const char* key : {"waypoints", "edges", "stations", "zones", "assignedRobots"}) {
        const json* value = field(body, key);
        if (value && !value->is_array()) {
            errors.push_back(std::format("{} must be an array", key));
        }
    }
    for (const char* key : {"width", "height"}) {
        if (!positive(field(body, key))) {
            errors.push_back(std::format("{} must be a positive number", key));
        }
    }
    const json* meta = field(body, "meta");
    if (!meta || !meta->is_object()) {
        errors.push_back("meta must be an object with resolution and origin");
    } else {
        if (!positive(field(*meta, "resolution"))) {
            errors.push_back("meta.resolution must be a positive number");
        }
        const json* origin = field(*meta, "origin");
        if (!origin || !origin->is_array() || origin->size() < 2 || origin->size() > 3 ||
            !std::ranges::all_of(*origin, [](const json& v) { return v.is_number(); })) {
            errors.push_back("meta.origin must be [x, y] or [x, y, theta] numbers");
        }
    }

    // Узлы: waypoints и stations — одно пространство id (на станцию тоже могут вести рёбра)
    std::set<std::string> node_ids;
    auto check_node = [&](const json& node, const char* kind) {
        const std::string* id = id_of(node);
        if (!id) {
            errors.push_back(std::format("{} without a string id", kind));
            return;
        }
        const json* u = field(node, "u");
        const json* v = field(node, "v");
        if (!u || !u->is_number() || !v || !v->is_number()) {
            errors.push_back(std::format("{} {}: u and v must be numbers", kind, *id));
        }
        if (!node_ids.insert(*id).second) {
            errors.push_back(std::format("duplicate node id: {}", *id));
        }
    };
    for (const auto& waypoint : list(body, "waypoints")) {
        check_node(waypoint, "waypoint");
    }
    for (const auto& station : list(body, "stations")) {
        check_node(station, "station");
    }

    std::set<std::string> edge_ids;
    std::set<std::string> connected;
    for (const auto& edge : list(body, "edges")) {
        const std::string* id = id_of(edge);
        if (!id) {
            errors.push_back("edge without a string id");
            continue;
        }
        if (!edge_ids.insert(*id).second) {
            errors.push_back(std::format("duplicate edge id: {}", *id));
        }
        for (const char* end : {"from", "to"}) {
            const json* node = field(edge, end);
            if (!node || !node->is_string() || !node_ids.contains(node->get<std::string>())) {
                errors.push_back(std::format("edge {}: {}-node not found", *id, end));
            } else {
                connected.insert(node->get<std::string>());
            }
        }
        const json* from = field(edge, "from");
        const json* to = field(edge, "to");
        if (from && to && *from == *to) {
            warnings.push_back(std::format("edge {}: self-loop, ignored for routes", *id));
        }
        if (const json* speed = field(edge, "maxSpeed"); speed && (!speed->is_number() || speed->get<double>() < 0)) {
            errors.push_back(std::format("edge {}: maxSpeed must be a number >= 0", *id));
        }
        if (const json* cost = field(edge, "cost"); cost && !cost->is_number()) {
            errors.push_back(std::format("edge {}: cost must be a number", *id));
        } else if (cost && cost->get<double>() < 0) {
            warnings.push_back(std::format("edge {}: cost < 0, used as 0", *id));
        }
    }

    for (const auto& waypoint : list(body, "waypoints")) {
        if (const std::string* id = id_of(waypoint); id && !connected.contains(*id)) {
            warnings.push_back(std::format("waypoint {} is isolated (no edges)", *id));
        }
    }
    for (const auto& station : list(body, "stations")) {
        const std::string* id = id_of(station);
        if (!id) {
            continue;
        }
        const json& nodes = list(station, "interactionNodeIds");
        if (nodes.empty()) {
            warnings.push_back(std::format("station {}: no interactionNodeIds, robots cannot reach it", *id));
        }
        for (const auto& node : nodes) {
            if (!node.is_string() || !node_ids.contains(node.get<std::string>())) {
                errors.push_back(std::format("station {}: interaction node {} not found", *id, node.dump()));
            }
        }
    }
    return check;
}


std::optional<uint32_t> Graph::find_node(const std::string& id) const {
    auto it = node_index.find(id);
    if (it == node_index.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<uint32_t> Graph::arrival_nodes(uint32_t node) const {
    const GraphNode& n = nodes[node];
    if (n.station && !n.has_edges) {
        return n.interaction_nodes;
    }
    return {node};
}

Graph build_graph(const std::string& map_id, uint32_t version, const json& body) {
    Graph graph{map_id, version, {}, {}, {}};
    const json& meta = body.at("meta");
    const double resolution = meta.at("resolution").get<double>();
    const double origin_x = meta.at("origin").at(0).get<double>();
    const double origin_y = meta.at("origin").at(1).get<double>();
    const double height = body.at("height").get<double>();

    auto add_node = [&](const json& item, bool station) {
        GraphNode node;
        node.id = item.at("id").get<std::string>();
        node.x = origin_x + item.at("u").get<double>() * resolution;
        node.y = origin_y + (height - item.at("v").get<double>()) * resolution;   // ось v картинки — вниз
        if (const json* theta = field(item, "theta"); theta && theta->is_number()) {
            node.theta = theta->get<double>();
        }
        node.station = station;
        graph.node_index.emplace(node.id, static_cast<uint32_t>(graph.nodes.size()));
        graph.nodes.push_back(std::move(node));
    };
    for (const auto& waypoint : list(body, "waypoints")) {
        add_node(waypoint, false);
    }
    for (const auto& station : list(body, "stations")) {
        add_node(station, true);
    }

    // Узлы взаимодействия — вторым проходом: станция может ссылаться на узел, описанный позже
    for (const auto& station : list(body, "stations")) {
        GraphNode& node = graph.nodes[graph.node_index.at(station.at("id").get<std::string>())];
        for (const auto& id : list(station, "interactionNodeIds")) {
            node.interaction_nodes.push_back(graph.node_index.at(id.get<std::string>()));
        }
    }

    for (const auto& item : list(body, "edges")) {
        GraphEdge edge;
        edge.id = item.at("id").get<std::string>();
        edge.from = graph.node_index.at(item.at("from").get<std::string>());
        edge.to = graph.node_index.at(item.at("to").get<std::string>());
        if (edge.from == edge.to) {
            continue;   // петля: по ней никуда не уехать
        }
        GraphNode& a = graph.nodes[edge.from];
        GraphNode& b = graph.nodes[edge.to];
        edge.length = std::hypot(b.x - a.x, b.y - a.y);
        if (const json* speed = field(item, "maxSpeed")) {
            edge.max_speed = speed->get<double>();
        }
        if (const json* cost = field(item, "cost")) {
            edge.cost = std::max(0.0, cost->get<double>());
        }
        a.has_edges = true;
        b.has_edges = true;
        graph.edges.push_back(std::move(edge));
    }
    return graph;
}