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

TEST_CASE("A waiting robot can approach the reserved crossing", "[traffic-approach]") {
    auto g = graph();
    g.nodes.push_back({"5"});
    g.node_index.emplace("5", 5);
    g.edges.push_back({"14", 1, 4, 1});
    g.edges.push_back({"45", 4, 5, 1});
    g.edges.push_back({"52", 5, 2, 1});
    g.edges.push_back({"20", 2, 0, 1});

    auto a = route("A", 0);
    a.nodes = {0, 2, 3};
    a.edges = {0, 1};
    auto b = route("B", 1);
    auto wanted = b;
    wanted.nodes = {1, 4, 5, 2, 0};
    wanted.edges = {4, 5, 6, 7};

    auto robots = occupants();
    std::vector<ProtectedRoute> held{a, b};
    FixedRouteGate gate;
    const auto can_wait = [](std::uint32_t) { return true; };

    SECTION("advance to the last safe node and extend when the crossing clears") {
        const auto approach = gate.plan_extension(g, robots, held, wanted, can_wait);
        REQUIRE(approach.granted);
        CHECK(approach.released_nodes == 3);
        CHECK(approach.reason == "node 2 is reserved by robot A");
        CHECK_FALSE(gate.check(g, robots, held, wanted).granted);

        // B уже у границы 5. Пока A держит узел 2, новой выдачи нет.
        held[1].nodes = {1, 4, 5};
        held[1].edges = {4, 5};
        held[1].reached = wanted.reached = 2;
        CHECK_FALSE(gate.plan_extension(g, robots, held, wanted, can_wait).granted);

        held[0].reached = 2; // A подтвердил узел 3 за пересечением.
        const auto rest = gate.plan_extension(g, robots, held, wanted, can_wait);
        CHECK(rest.granted);
        CHECK(rest.released_nodes == 5);
        CHECK(rest.reason.empty());
    }
    SECTION("the closest stop would block another route") {
        const auto approach = gate.plan_extension(g, robots, held, wanted,
            [](std::uint32_t node) { return node == 4; });
        CHECK(approach.granted);
        CHECK(approach.released_nodes == 2);
    }
    SECTION("there is no safe stop") {
        const auto approach = gate.plan_extension(g, robots, held, wanted,
            [](std::uint32_t) { return false; });
        CHECK_FALSE(approach.granted);
        CHECK(approach.released_nodes == 1);
    }
    SECTION("the other robot's location is unknown") {
        robots[0].map.clear();
        const auto approach = gate.plan_extension(g, robots, held, wanted, can_wait);
        CHECK_FALSE(approach.granted);
        CHECK(approach.released_nodes == 0);
    }
}
