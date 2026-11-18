#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>
#include <vector>

#include "traffic/Reservations.hpp"
#include "traffic/ResourceMap.hpp"

namespace {

using traffic::ResourceId;
using traffic::RobotId;

constexpr RobotId robot1 = 1;
constexpr RobotId robot2 = 2;

enum Node : std::uint32_t {
    a,
    b,
    c,
    d
};

enum Edge : std::uint32_t {
    ab,
    ba,
    bc,
    cb,
    bd,
    db,
    ab_parallel
};

Graph cross_map() {
    Graph graph;
    graph.map_id = "test-traffic";
    graph.version = 1;

    for (const char* id : {"a", "b", "c", "d"}) {
        GraphNode node;
        node.id = id;
        graph.nodes.push_back(node);
    }

    auto edge = [&](const char* id,
                    std::uint32_t from,
                    std::uint32_t to) {
        GraphEdge item;
        item.id = id;
        item.from = from;
        item.to = to;
        item.length = 1;
        graph.edges.push_back(item);
    };

    edge("ab", a, b);
    edge("ba", b, a);
    edge("bc", b, c);
    edge("cb", c, b);
    edge("bd", b, d);
    edge("db", d, b);

    // Другой edgeId между той же парой узлов —
    // пока тот же физический проезд.
    edge("ab-parallel", a, b);

    return graph;
}

}  // namespace

TEST_CASE("traffic: reverse and parallel edges share a lane",
          "[traffic]") {
    traffic::ResourceMap map(cross_map());

    CHECK(map.size() == 7);  // четыре узла и три проезда
    CHECK(map.map_id() == "test-traffic");
    CHECK(map.version() == 1);

    CHECK(map.lane_resource(ab) == map.lane_resource(ba));
    CHECK(map.lane_resource(ab) == map.lane_resource(ab_parallel));

    CHECK(map.lane_resource(ab) != map.lane_resource(bc));
    CHECK(map.lane_resource(ab) != map.lane_resource(bd));

    for (std::uint32_t node = a; node <= d; ++node) {
        CHECK(map.node_resource(node) != map.lane_resource(ab));
    }

    CHECK(map.node_resource(a) != map.node_resource(b));
}

TEST_CASE("traffic: resources own their map data", "[traffic]") {
    Graph graph = cross_map();
    traffic::ResourceMap map(graph);

    graph.map_id = "changed";
    graph.version = 2;
    graph.nodes.clear();
    graph.edges.clear();

    CHECK(map.map_id() == "test-traffic");
    CHECK(map.version() == 1);
    CHECK(map.size() == 7);
    CHECK(map.lane_resource(ab) == map.lane_resource(ba));
    CHECK(map.node_resource(d) == d);
}

TEST_CASE("traffic: empty graph and isolated node", "[traffic]") {
    Graph graph;
    graph.map_id = "empty";
    graph.version = 1;

    traffic::ResourceMap empty(graph);
    CHECK(empty.size() == 0);
    CHECK_THROWS_AS(empty.node_resource(0), std::out_of_range);
    CHECK_THROWS_AS(empty.lane_resource(0), std::out_of_range);

    GraphNode node;
    node.id = "alone";
    graph.nodes.push_back(node);

    traffic::ResourceMap isolated(graph);
    CHECK(isolated.size() == 1);

    traffic::Reservations reservations(isolated);
    const std::array resources = {isolated.node_resource(0)};

    CHECK_FALSE(reservations.try_acquire(robot1, resources));
    CHECK(reservations.owner(resources[0]) == robot1);
}

TEST_CASE("traffic: invalid graph endpoints are rejected",
          "[traffic]") {
    Graph graph = cross_map();

    SECTION("endpoint outside graph") {
        graph.edges[ab].to = 100;
    }

    SECTION("self-loop") {
        graph.edges[ab].to = a;
    }

    CHECK_THROWS_AS(
        traffic::ResourceMap{graph},
        std::invalid_argument);
}

TEST_CASE("traffic: opposing robots cannot share a lane",
          "[traffic]") {
    traffic::Reservations reservations{
        traffic::ResourceMap{cross_map()}};

    const auto& map = reservations.resources();

    const std::array forward = {map.lane_resource(ab)};
    const std::array reverse = {map.lane_resource(ba)};
    const std::array following = {map.lane_resource(ab_parallel)};

    CHECK_FALSE(reservations.try_acquire(robot1, forward));

    auto opposing = reservations.try_acquire(robot2, reverse);
    REQUIRE(opposing);
    CHECK(opposing->resource == forward[0]);
    CHECK(opposing->owner == robot1);

    // В первой версии движение следом по тому же проезду
    // также ждёт освобождения.
    CHECK(reservations.try_acquire(robot2, following).has_value());

    CHECK(reservations.release(robot1, forward));
    CHECK_FALSE(reservations.try_acquire(robot2, reverse));
}

TEST_CASE("traffic: standing robot blocks an arrival node",
          "[traffic]") {
    traffic::Reservations reservations{
        traffic::ResourceMap{cross_map()}};

    const auto& map = reservations.resources();

    const std::array standing = {map.node_resource(b)};
    REQUIRE_FALSE(reservations.try_acquire(robot1, standing));

    // Робот 2 хочет пройти c → b.
    const std::array move = {
        map.lane_resource(cb),
        map.node_resource(b)
    };

    auto conflict = reservations.try_acquire(robot2, move);

    REQUIRE(conflict);
    CHECK(conflict->resource == map.node_resource(b));
    CHECK(conflict->owner == robot1);

    // Проезд был свободен, но запрос не должен захватить
    // его частично перед отказом на узле.
    CHECK_FALSE(reservations.owner(map.lane_resource(cb)));
}

TEST_CASE("traffic: independent resources can be held together",
          "[traffic]") {
    traffic::Reservations reservations{
        traffic::ResourceMap{cross_map()}};

    const auto& map = reservations.resources();

    const std::array first = {
        map.node_resource(a),
        map.lane_resource(ab)
    };
    const std::array second = {
        map.node_resource(c),
        map.lane_resource(cb)
    };

    CHECK_FALSE(reservations.try_acquire(robot1, first));
    CHECK_FALSE(reservations.try_acquire(robot2, second));

    CHECK(reservations.owner(map.node_resource(a)) == robot1);
    CHECK(reservations.owner(map.node_resource(c)) == robot2);

    // Наборы здесь проверяют независимость ресурсов.
    // Выдачу реального движения через общий b будет проверять
    // контроллер, запрашивающий также ресурс назначения b.
}

TEST_CASE("traffic: failed acquire preserves old ownership",
          "[traffic]") {
    traffic::Reservations reservations{
        traffic::ResourceMap{cross_map()}};

    const auto& map = reservations.resources();

    const std::array own = {map.node_resource(a)};
    const std::array occupied = {map.node_resource(b)};

    REQUIRE_FALSE(reservations.try_acquire(robot1, own));
    REQUIRE_FALSE(reservations.try_acquire(robot2, occupied));

    const std::array wanted = {
        map.node_resource(a),     // уже наше
        map.lane_resource(ab),    // свободно
        map.node_resource(b)      // занято
    };

    auto conflict = reservations.try_acquire(robot1, wanted);

    REQUIRE(conflict);
    CHECK(conflict->owner == robot2);

    CHECK(reservations.owner(map.node_resource(a)) == robot1);
    CHECK_FALSE(reservations.owner(map.lane_resource(ab)));
    CHECK(reservations.owner(map.node_resource(b)) == robot2);
}

TEST_CASE("traffic: repeated acquire is idempotent",
          "[traffic]") {
    traffic::Reservations reservations{
        traffic::ResourceMap{cross_map()}};

    const auto& map = reservations.resources();
    const auto node = map.node_resource(a);

    // Повтор в одном запросе тоже допустим.
    const std::array wanted = {node, node};

    CHECK_FALSE(reservations.try_acquire(robot1, wanted));
    CHECK_FALSE(reservations.try_acquire(robot1, wanted));
    CHECK(reservations.owner(node) == robot1);

    CHECK(reservations.release(robot1, wanted));
    CHECK_FALSE(reservations.owner(node));

    CHECK(reservations.release(robot1, wanted));
}

TEST_CASE("traffic: release cannot remove another robot's reserve",
          "[traffic]") {
    traffic::Reservations reservations{
        traffic::ResourceMap{cross_map()}};

    const auto& map = reservations.resources();

    const std::array first = {map.node_resource(a)};
    const std::array second = {map.node_resource(b)};

    REQUIRE_FALSE(reservations.try_acquire(robot1, first));
    REQUIRE_FALSE(reservations.try_acquire(robot2, second));

    const std::array mixed = {
        map.node_resource(a),
        map.node_resource(b)
    };

    CHECK_FALSE(reservations.release(robot1, mixed));

    // Освобождение тоже не должно быть частичным.
    CHECK(reservations.owner(map.node_resource(a)) == robot1);
    CHECK(reservations.owner(map.node_resource(b)) == robot2);
}

TEST_CASE("traffic: releasing the path keeps the standing node",
          "[traffic]") {
    traffic::Reservations reservations{
        traffic::ResourceMap{cross_map()}};

    const auto& map = reservations.resources();

    const std::array move = {
        map.node_resource(a),
        map.lane_resource(ab),
        map.node_resource(b)
    };

    REQUIRE_FALSE(reservations.try_acquire(robot1, move));

    // Подтверждено достижение b: освобождаем a и проезд.
    const std::array passed = {
        map.node_resource(a),
        map.lane_resource(ab)
    };

    CHECK(reservations.release(robot1, passed));

    CHECK_FALSE(reservations.owner(map.node_resource(a)));
    CHECK_FALSE(reservations.owner(map.lane_resource(ab)));
    CHECK(reservations.owner(map.node_resource(b)) == robot1);

    const std::array destination = {map.node_resource(b)};
    CHECK(reservations.try_acquire(robot2, destination).has_value());
}

TEST_CASE("traffic: invalid resource does not change reservations",
          "[traffic]") {
    traffic::Reservations reservations{
        traffic::ResourceMap{cross_map()}};

    const auto& map = reservations.resources();
    const auto node = map.node_resource(a);
    const auto invalid = static_cast<ResourceId>(map.size());

    const std::array mixed = {node, invalid};

    CHECK_THROWS_AS(
        reservations.try_acquire(robot1, mixed),
        std::out_of_range);

    CHECK_FALSE(reservations.owner(node));

    const std::array valid = {node};
    REQUIRE_FALSE(reservations.try_acquire(robot1, valid));

    CHECK_THROWS_AS(
        reservations.release(robot1, mixed),
        std::out_of_range);

    CHECK(reservations.owner(node) == robot1);
    CHECK_THROWS_AS(reservations.owner(invalid), std::out_of_range);
}

TEST_CASE("traffic: map versions have independent tables",
          "[traffic]") {
    Graph graph = cross_map();

    traffic::Reservations first{
        traffic::ResourceMap{graph}};

    graph.version = 2;
    traffic::Reservations second{
        traffic::ResourceMap{graph}};

    const std::array first_node = {
        first.resources().node_resource(a)
    };
    const std::array second_node = {
        second.resources().node_resource(a)
    };

    CHECK(first.resources().version() == 1);
    CHECK(second.resources().version() == 2);

    CHECK_FALSE(first.try_acquire(robot1, first_node));
    CHECK_FALSE(second.try_acquire(robot2, second_node));

    CHECK(first.owner(first_node[0]) == robot1);
    CHECK(second.owner(second_node[0]) == robot2);
}