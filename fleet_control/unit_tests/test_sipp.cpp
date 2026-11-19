#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

#include "traffic/SippPlanner.hpp"

namespace {

using namespace traffic;

Graph graph_with_nodes(std::uint32_t count) {
    Graph graph;
    graph.map_id = "test-sipp";
    graph.version = 1;
    for (std::uint32_t i = 0; i < count; ++i) {
        GraphNode node;
        node.id = std::to_string(i);
        graph.nodes.push_back(node);
    }
    return graph;
}

void edge(Graph& graph, std::uint32_t from, std::uint32_t to, double length = 1) {
    GraphEdge e;
    e.id = std::to_string(graph.edges.size());
    e.from = from;
    e.to = to;
    e.length = length;
    graph.edges.push_back(e);
}

// 0 --1м--> 1 --1м--> 2
Graph line() {
    auto graph = graph_with_nodes(3);
    edge(graph, 0, 1);
    edge(graph, 1, 2);
    return graph;
}

SippRequest request(std::uint32_t from = 0, std::uint32_t goal = 2) {
    SippRequest r;
    r.robot = 1;
    r.from = from;
    r.goals = {goal};
    r.latest_arrival = 10'000;
    return r;
}

void reserve(ReservationTable& table, ResourceId resource, Interval time) {
    const std::vector<TimedReservation> plan = {{resource, time}};
    REQUIRE_FALSE(table.try_replace(2, plan));
}

std::vector<std::uint32_t> path(const SippResult& result) {
    std::vector<std::uint32_t> nodes;
    for (const auto& visit : result.visits) nodes.push_back(visit.node);
    return nodes;
}

void accept(ReservationTable& table, const SippRequest& r, const SippResult& result) {
    REQUIRE(result.status == SippStatus::Found);
    REQUIRE_FALSE(result.visits.empty());
    CHECK(result.visits.front().node == r.from);
    CHECK(result.visits.front().arrival == r.start_time);
    CHECK(result.visits.back().departure == kForever);
    CHECK(result.visits.back().arrival <= r.latest_arrival);
    CHECK(result.edges.size() + 1 == result.visits.size());
    CHECK_FALSE(table.try_replace(r.robot, result.reservations));
}

}  // namespace

TEST_CASE("SIPP: free path and generated reservations", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    SippPlanner planner(graph, 1);
    auto r = request();
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(path(result) == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(result.visits[0].departure == 0);
    CHECK(result.visits[1].arrival == 1000);
    CHECK(result.visits[1].departure == 1000);
    CHECK(result.visits[2].arrival == 2000);

    // Поиск сам таблицу не изменил.
    CHECK(table.safe_intervals(table.resources().node_resource(0), {0, 10}) ==
          std::vector<Interval>{{0, 10}});
    accept(table, r, result);
    CHECK(table.safe_intervals(table.resources().node_resource(1), {1000, 1001}).empty());
    CHECK(table.safe_intervals(table.resources().node_resource(2), {2000, kForever}).empty());
}

TEST_CASE("SIPP: wait before an occupied node", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    reserve(table, table.resources().node_resource(1), {1000, 3000});
    SippPlanner planner(graph, 1);
    auto r = request();
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(result.visits[0].departure == 2000);
    CHECK(result.visits[1].arrival == 3000);
    CHECK(result.visits[2].arrival == 4000);
    accept(table, r, result);
}

TEST_CASE("SIPP: wait for the shared opposing lane", "[sipp]") {
    auto graph = line();
    edge(graph, 1, 0);
    ReservationTable table{ResourceMap{graph}};
    reserve(table, table.resources().lane_resource(2), {0, 1500});
    SippPlanner planner(graph, 1);
    auto r = request();
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(result.visits[0].departure == 1500);
    CHECK(result.visits.back().arrival == 3500);
    accept(table, r, result);
}

TEST_CASE("SIPP: choose a detour instead of a long wait", "[sipp]") {
    // 0 -> 1 -> 2; объезд 0 -> 3 -> 2.
    auto graph = graph_with_nodes(4);
    edge(graph, 0, 1);
    edge(graph, 1, 2);
    edge(graph, 0, 3, 2);
    edge(graph, 3, 2, 2);
    ReservationTable table{ResourceMap{graph}};
    reserve(table, table.resources().node_resource(1), {0, 9000});
    SippPlanner planner(graph, 1);
    auto r = request();
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(path(result) == std::vector<std::uint32_t>{0, 3, 2});
    CHECK(result.visits.back().arrival == 4000);
    accept(table, r, result);
}

TEST_CASE("SIPP: no waiting through an unsafe source interval", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    const std::vector<TimedReservation> other = {
        {table.resources().node_resource(0), {1000, kForever}},
        {table.resources().lane_resource(0), {0, 2000}}
    };
    REQUIRE_FALSE(table.try_replace(2, other));
    SippPlanner planner(graph, 1);
    auto result = planner.search(table, request());
    CHECK(result.status == SippStatus::NoPathWithinWindow);
    CHECK(result.reservations.empty());
}

TEST_CASE("SIPP: occupied start cannot appear later", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    reserve(table, table.resources().node_resource(0), {0, 10});
    SippPlanner planner(graph, 1);
    auto result = planner.search(table, request());
    CHECK(result.status == SippStatus::StartBlocked);
    CHECK(result.expanded == 0);
}

TEST_CASE("SIPP: future goal reservation beyond window still matters", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    reserve(table, table.resources().node_resource(2), {20'000, 30'000});
    SippPlanner planner(graph, 1);
    auto result = planner.search(table, request());
    CHECK(result.status == SippStatus::NoPathWithinWindow);
}

TEST_CASE("SIPP: can wait for the infinite goal interval", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    reserve(table, table.resources().node_resource(2), {2000, 5000});
    SippPlanner planner(graph, 1);
    auto r = request();
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(result.visits.back().arrival == 5000);
    accept(table, r, result);
}

TEST_CASE("SIPP: may leave a finite goal interval and return", "[sipp]") {
    auto graph = graph_with_nodes(2);
    edge(graph, 0, 1);
    edge(graph, 1, 0);
    ReservationTable table{ResourceMap{graph}};
    reserve(table, table.resources().node_resource(0), {1000, 3000});
    SippPlanner planner(graph, 1);
    auto r = request(0, 0);
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(path(result) == std::vector<std::uint32_t>{0, 1, 0});
    CHECK(result.visits.back().arrival == 3000);
    accept(table, r, result);
}

TEST_CASE("SIPP: start already at a permanently free goal", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    SippPlanner planner(graph, 1);
    auto r = request(1, 1);
    r.start_time = 50;
    r.latest_arrival = 50;
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(result.visits.size() == 1);
    CHECK(result.visits.front().arrival == 50);
    accept(table, r, result);
}

TEST_CASE("SIPP: ignore the robot's old plan during search", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    const std::vector<TimedReservation> old = {
        {table.resources().node_resource(0), {0, kForever}}
    };
    REQUIRE_FALSE(table.try_replace(1, old));
    SippPlanner planner(graph, 1);
    auto r = request();
    auto result = planner.search(table, r);
    accept(table, r, result);
    CHECK(table.safe_intervals(table.resources().node_resource(0), {1, 10}) ==
          std::vector<Interval>{{1, 10}});
}

TEST_CASE("SIPP: choose among multiple physical goals", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    reserve(table, table.resources().node_resource(1), {0, kForever});
    SippPlanner planner(graph, 1);
    auto r = request();
    r.goals = {1, 0};
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(result.visits.back().node == 0);
    accept(table, r, result);
}

TEST_CASE("SIPP: limits do not produce a partial plan", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    SippPlanner planner(graph, 1);
    auto r = request();
    SECTION("arrival limit") {
        r.latest_arrival = 1999;
        auto result = planner.search(table, r);
        CHECK(result.status == SippStatus::NoPathWithinWindow);
        CHECK(result.visits.empty());
        CHECK(result.reservations.empty());
    }
    SECTION("expansion limit") {
        r.max_expansions = 1;
        auto result = planner.search(table, r);
        CHECK(result.status == SippStatus::ExpansionLimit);
        CHECK(result.expanded == 1);
        CHECK(result.visits.empty());
        CHECK(result.reservations.empty());
    }
}

TEST_CASE("SIPP: time rounding, edge speed and cost", "[sipp]") {
    auto graph = graph_with_nodes(2);
    edge(graph, 0, 1);
    Time expected = 1000;
    SECTION("speed of the edge") {
        graph.edges[0].max_speed = 2;
        expected = 500;
    }
    SECTION("round up") {
        graph.edges[0].max_speed = 3;
        expected = 334;
    }
    SECTION("zero length still takes one tick") {
        graph.edges[0].length = 0;
        expected = 1;
    }
    SECTION("cost is not physical time") {
        graph.edges[0].cost = 100;
    }
    ReservationTable table{ResourceMap{graph}};
    SippPlanner planner(graph, 1);
    auto r = request(0, 1);
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(result.visits.back().arrival == expected);
    accept(table, r, result);
}

TEST_CASE("SIPP: zero-length edges cannot bypass a node reservation", "[sipp]") {
    auto graph = graph_with_nodes(3);
    edge(graph, 0, 1, 0);
    edge(graph, 1, 2, 0);
    ReservationTable table{ResourceMap{graph}};
    reserve(table, table.resources().node_resource(1), {0, 2});
    SippPlanner planner(graph, 1);
    auto r = request();
    auto result = planner.search(table, r);
    REQUIRE(result.status == SippStatus::Found);
    CHECK(result.visits[1].arrival == 2);
    CHECK(result.visits.back().arrival == 3);
    accept(table, r, result);
}

TEST_CASE("SIPP: different map versions are rejected", "[sipp]") {
    auto graph = line();
    SippPlanner planner(graph, 1);
    graph.version = 2;
    ReservationTable table{ResourceMap{graph}};
    CHECK_THROWS_AS(planner.search(table, request()), std::invalid_argument);
}

TEST_CASE("SIPP: invalid parameters are rejected", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    SippPlanner planner(graph, 1);
    auto r = request();
    SECTION("negative start") { r.start_time = -1; }
    SECTION("infinite arrival bound") { r.latest_arrival = kForever; }
    SECTION("arrival before start") { r.start_time = r.latest_arrival + 1; }
    SECTION("no goals") { r.goals.clear(); }
    SECTION("no expansion budget") { r.max_expansions = 0; }
    CHECK_THROWS_AS(planner.search(table, r), std::invalid_argument);
}

TEST_CASE("SIPP: invalid graph speeds and lengths are rejected", "[sipp]") {
    auto graph = line();
    SECTION("zero default speed") {
        CHECK_THROWS_AS(SippPlanner(graph, 0), std::invalid_argument);
        return;
    }
    SECTION("negative length") { graph.edges[0].length = -1; }
    SECTION("infinite length") { graph.edges[0].length = std::numeric_limits<double>::infinity(); }
    SECTION("NaN speed") { graph.edges[0].max_speed = std::numeric_limits<double>::quiet_NaN(); }
    CHECK_THROWS_AS(SippPlanner(graph, 1), std::invalid_argument);
}

TEST_CASE("SIPP: time addition near the limit does not overflow", "[sipp]") {
    auto graph = line();
    ReservationTable table{ResourceMap{graph}};
    SippPlanner planner(graph, 1);
    auto r = request();
    r.start_time = kForever - 100;
    r.latest_arrival = kForever - 1;
    auto result = planner.search(table, r);
    CHECK(result.status == SippStatus::NoPathWithinWindow);
}

namespace {

// Независимый эталон: перебираем каждую миллисекунду,
// узел и возможное действие. Здесь нет безопасных интервалов как состояний.
std::optional<Time> grid_search(
    const Graph& graph, const ReservationTable& table, const SippRequest& r) {

    auto free = [&](ResourceId resource, Time begin, Time end) {
        return table.safe_intervals(resource, {begin, end}, r.robot) ==
               std::vector<Interval>{{begin, end}};
    };
    if (!free(table.resources().node_resource(r.from), r.start_time, r.start_time + 1)) {
        return std::nullopt;
    }
    std::vector<std::vector<bool>> reachable(
        r.latest_arrival + 1, std::vector<bool>(graph.nodes.size(), false));
    reachable[r.start_time][r.from] = true;

    for (Time time = r.start_time; time <= r.latest_arrival; ++time) {
        for (std::uint32_t node = 0; node < graph.nodes.size(); ++node) {
            if (!reachable[time][node]) continue;
            if (std::find(r.goals.begin(), r.goals.end(), node) != r.goals.end() &&
                free(table.resources().node_resource(node), time, kForever)) {
                return time;
            }
            if (time < r.latest_arrival &&
                free(table.resources().node_resource(node), time + 1, time + 2)) {
                reachable[time + 1][node] = true;
            }
            for (std::uint32_t e = 0; e < graph.edges.size(); ++e) {
                const auto& arc = graph.edges[e];
                if (arc.from != node) continue;
                // В случайных графах length=1..4, speed=1024:
                // длительности соответственно 1..4 мс.
                const Time duration = static_cast<Time>(arc.length);
                const Time arrival = time + duration;
                if (arrival > r.latest_arrival) continue;
                if (free(table.resources().lane_resource(e), time, arrival) &&
                    free(table.resources().node_resource(arc.to), arrival, arrival + 1)) {
                    reachable[arrival][arc.to] = true;
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("SIPP: random maps match time-expanded exhaustive search", "[sipp]") {
    std::mt19937 random(20261119);
    for (int iteration = 0; iteration < 200; ++iteration) {
        CAPTURE(iteration);
        auto graph = graph_with_nodes(5);
        for (std::uint32_t from = 0; from < 5; ++from) {
            for (std::uint32_t to = 0; to < 5; ++to) {
                if (from == to || random() % 3 != 0) continue;
                edge(graph, from, to, 1 + random() % 4);
                graph.edges.back().max_speed = 1024;
            }
        }
        ReservationTable table{ResourceMap{graph}};
        for (int i = 0; i < 12; ++i) {
            const auto resource = static_cast<ResourceId>(random() % table.resources().size());
            const Time begin = random() % 25;
            const Time end = random() % 5 == 0 ? kForever : begin + 1 + random() % 5;
            const std::vector<TimedReservation> plan = {{resource, {begin, end}}};
            (void)table.try_replace(100 + i, plan);
        }
        SippPlanner planner(graph, 1);
        auto r = request(0, 4);
        r.latest_arrival = 20;
        const auto expected = grid_search(graph, table, r);
        const auto result = planner.search(table, r);
        REQUIRE((result.status == SippStatus::Found) == expected.has_value());
        if (expected) {
            CHECK(result.visits.back().arrival == *expected);
            accept(table, r, result);
        } else {
            CHECK(result.status != SippStatus::ExpansionLimit);
        }
    }
}
