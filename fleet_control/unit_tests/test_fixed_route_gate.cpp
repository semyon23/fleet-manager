#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "traffic/FixedRouteGate.hpp"

namespace {

using namespace traffic;

Graph graph() {
    Graph g;
    g.map_id = "test";
    g.version = 1;

    for (std::uint32_t i = 0; i < 5; ++i) {
        GraphNode n;
        n.id = std::to_string(i);
        g.node_index[n.id] = i;
        g.nodes.push_back(n);
    }

    for (auto [a, b] : {
            std::pair{0, 2}, {2, 3}, {1, 2}, {2, 4}}) {
        GraphEdge e;
        e.from = a;
        e.to = b;
        e.length = 1;
        e.id = std::to_string(g.edges.size());
        g.edges.push_back(e);
    }

    return g;
}

ProtectedRoute route(
    std::string robot,
    std::uint32_t start) {

    return {
        robot, robot + "/s0", "test", 1,
        {start}, {}, 0
    };
}

std::vector<TrafficOccupant> occupants() {
    return {
        {"A", "test", "0", true, true},
        {"B", "test", "1", true, true}
    };
}

} // namespace

TEST_CASE(
    "fixed traffic: crossing waits then clears on indexed progress",
    "[traffic-live]") {

    auto g = graph();
    FixedRouteGate gate;

    auto a = route("A", 0);
    auto b = route("B", 1);

    auto want_a = a;
    want_a.nodes = {0, 2, 3};
    want_a.edges = {0, 1};

    auto want_b = b;
    want_b.nodes = {1, 2, 4};
    want_b.edges = {2, 3};

    auto robots = occupants();
    std::vector<ProtectedRoute> held = {a, b};

    REQUIRE(gate.check(g, robots, held, want_a).granted);

    held[0] = want_a;
    CHECK_FALSE(gate.check(g, robots, held, want_b).granted);

    // A достиг пересечения — B ещё ждёт.
    held[0].reached = 1;
    CHECK_FALSE(gate.check(g, robots, held, want_b).granted);

    // A подтвердил следующий узел — пересечение свободно.
    held[0].reached = 2;
    CHECK(gate.check(g, robots, held, want_b).granted);
}

TEST_CASE(
    "fixed traffic: stationary robot without mission blocks route",
    "[traffic-live]") {

    auto g = graph();
    FixedRouteGate gate;

    auto a = route("A", 0);
    auto wanted = a;
    wanted.nodes = {0, 2, 3};
    wanted.edges = {0, 1};

    std::vector<ProtectedRoute> held = {a};
    auto robots = occupants();

    robots[1] = {"B", "test", "2", true, false};
    CHECK_FALSE(gate.check(g, robots, held, wanted).granted);

    robots[1].node = "4";
    CHECK(gate.check(g, robots, held, wanted).granted);
}

TEST_CASE(
    "fixed traffic: lost connection retains issued base",
    "[traffic-live]") {

    auto g = graph();
    FixedRouteGate gate;

    auto a = route("A", 0);
    auto b = route("B", 1);

    a.nodes = {0, 2, 3};
    a.edges = {0, 1};

    auto wanted = b;
    wanted.nodes = {1, 2, 4};
    wanted.edges = {2, 3};

    std::vector<ProtectedRoute> held = {a, b};
    auto robots = occupants();

    robots[0].stationary = false;
    robots[0].node.clear();

    CHECK_FALSE(gate.check(g, robots, held, wanted).granted);
}

TEST_CASE(
    "fixed traffic: unknown position and external mission block grants",
    "[traffic-live]") {

    auto g = graph();
    FixedRouteGate gate;

    auto a = route("A", 0);
    auto wanted = a;
    wanted.nodes = {0, 2, 3};
    wanted.edges = {0, 1};

    std::vector<ProtectedRoute> held = {a};
    auto robots = occupants();

    SECTION("unknown map") {
        robots[1].map.clear();
    }

    SECTION("unknown node") {
        robots[1].node.clear();
        robots[1].has_mission = false;
    }

    SECTION("unknown active base") {
    }

    CHECK_FALSE(gate.check(g, robots, held, wanted).granted);
}

TEST_CASE(
    "fixed traffic: protected prefix and version cannot change",
    "[traffic-live]") {

    auto g = graph();
    FixedRouteGate gate;

    auto a = route("A", 0);
    auto wanted = a;
    wanted.nodes = {0, 2, 3};
    wanted.edges = {0, 1};

    std::vector<ProtectedRoute> held = {a};
    auto robots = occupants();
    robots.pop_back();

    SECTION("version") {
        wanted.version = 2;
    }

    SECTION("key") {
        wanted.key = "other/s0";
    }

    SECTION("node") {
        wanted.nodes = {1, 2, 4};
        wanted.edges = {2, 3};
    }

    CHECK_THROWS_AS(
        gate.check(g, robots, held, wanted),
        std::invalid_argument);
}