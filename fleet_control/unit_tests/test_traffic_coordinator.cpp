#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <stdexcept>
#include <vector>

#include "traffic/TrafficCoordinator.hpp"

namespace {
using namespace traffic;

Graph graph(std::uint32_t count) {
    Graph g;
    g.map_id = "coordination-test";
    g.version = 1;
    for (std::uint32_t i = 0; i < count; ++i) {
        GraphNode n;
        n.id = std::to_string(i);
        g.nodes.push_back(n);
    }
    return g;
}

void edge(Graph& g, std::uint32_t from, std::uint32_t to) {
    GraphEdge e;
    e.id = std::to_string(g.edges.size());
    e.from = from;
    e.to = to;
    e.length = 1;
    g.edges.push_back(e);
}

Graph crossing() {
    auto g = graph(5);
    edge(g, 0, 2); edge(g, 2, 3);
    edge(g, 1, 2); edge(g, 2, 4);
    return g;
}

CoordinationRequest request(
    RobotId robot, std::uint32_t from, std::uint32_t goal) {

    CoordinationRequest r;
    r.robot = robot;
    r.from = from;
    r.goals = {goal};
    r.latest_arrival = 100'000;
    return r;
}

void hold(ReservationTable& t, RobotId robot, std::uint32_t node) {
    const std::vector<TimedReservation> p = {
        {t.resources().node_resource(node), {0, kForever}}};
    REQUIRE_FALSE(t.try_replace(robot, p));
}

void same(const ReservationTable& a, const ReservationTable& b) {
    for (ResourceId id = 0; id < a.resources().size(); ++id) {
        CHECK(a.safe_intervals(id, {0, kForever}) ==
              b.safe_intervals(id, {0, kForever}));
        for (RobotId robot : {1, 2, 99}) {
            CHECK(a.safe_intervals(id, {0, kForever}, robot) ==
                  b.safe_intervals(id, {0, kForever}, robot));
        }
    }
}

void compatible(const Graph& g, const CoordinationResult& result) {
    ReservationTable check{ResourceMap{g}};
    for (const auto& plan : result.plans) {
        CHECK(plan.path.status == SippStatus::Found);
        REQUIRE_FALSE(check.try_replace(plan.robot, plan.path.reservations));
    }
}
} // namespace

TEST_CASE("coordinator: crossing has compatible plans", "[traffic-coordinator]") {
    auto g = crossing();
    ReservationTable t{ResourceMap{g}};
    hold(t, 1, 0); hold(t, 2, 1);
    TrafficCoordinator c(g, 1);
    const std::vector requests = {request(1, 0, 3), request(2, 1, 4)};

    auto result = c.replan(t, 0, requests);

    REQUIRE(result.status == CoordinationStatus::Accepted);
    REQUIRE(result.plans.size() == 2);
    CHECK(result.attempts == 1);
    CHECK(result.plans[0].path.visits.back().arrival == 2000);
    CHECK(result.plans[1].path.visits.back().arrival == 2001);
    compatible(g, result);
}

TEST_CASE("coordinator: priority aging and deterministic ties", "[traffic-coordinator]") {
    auto g = crossing();
    ReservationTable t{ResourceMap{g}};
    TrafficCoordinator c(g, 1);
    std::vector requests = {request(2, 1, 4), request(1, 0, 3)};
    Time now = 0;
    RobotId first = 1;

    SECTION("robot id tie") {}
    SECTION("mission priority") {
        requests[0].priority = 3;
        first = 2;
    }
    SECTION("older waiting") {
        now = 100;
        requests[1].waiting_since = 10;
        first = 2;
    }
    SECTION("aging overtakes fresh critical mission") {
        now = 120'000;
        requests[0].priority = 0;
        requests[1].priority = 3;
        requests[1].waiting_since = now;
        for (auto& r : requests) r.latest_arrival = now + 10'000;
        first = 2;
    }

    auto result = c.replan(t, now, requests);

    REQUIRE(result.status == CoordinationStatus::Accepted);
    CHECK(result.plans.front().robot == first);
}

TEST_CASE("coordinator: failed robot moves ahead of blocking goal", "[traffic-coordinator]") {
    auto g = graph(4);
    edge(g, 0, 2); edge(g, 1, 2); edge(g, 2, 3);
    ReservationTable t{ResourceMap{g}};
    TrafficCoordinator c(g, 1);
    const std::vector requests = {request(1, 0, 2), request(2, 1, 3)};

    auto result = c.replan(t, 0, requests);

    REQUIRE(result.status == CoordinationStatus::Accepted);
    CHECK(result.attempts == 2);
    CHECK(result.plans.front().robot == 2);
    CHECK_FALSE(result.blocked_robot);
    CHECK_FALSE(result.search_status);
    compatible(g, result);
}

TEST_CASE("coordinator: unplanned robot occupies its start", "[traffic-coordinator]") {
    auto g = graph(3);
    edge(g, 0, 1); edge(g, 1, 2);
    ReservationTable t{ResourceMap{g}};
    TrafficCoordinator c(g, 1);
    const std::vector requests = {request(1, 0, 1), request(2, 1, 2)};

    auto result = c.replan(t, 0, requests);

    REQUIRE(result.status == CoordinationStatus::Accepted);
    CHECK(result.attempts == 2);
    CHECK(result.plans.front().robot == 2);
    compatible(g, result);
}

TEST_CASE("coordinator: impossible swap preserves every old reservation", "[traffic-coordinator]") {
    auto g = graph(2);
    edge(g, 0, 1); edge(g, 1, 0);
    ReservationTable t{ResourceMap{g}};
    hold(t, 1, 0); hold(t, 2, 1);
    const auto before = t;
    TrafficCoordinator c(g, 1);
    const std::vector requests = {request(1, 0, 1), request(2, 1, 0)};

    auto result = c.replan(t, 0, requests);

    CHECK(result.status == CoordinationStatus::NoPlanFound);
    CHECK(result.attempts == 2);
    CHECK(result.plans.empty());
    same(t, before);
}

TEST_CASE("coordinator: budgets roll back partial success", "[traffic-coordinator]") {
    auto g = graph(4);
    edge(g, 0, 2); edge(g, 1, 2); edge(g, 2, 3);
    ReservationTable t{ResourceMap{g}};
    hold(t, 1, 0); hold(t, 2, 1);
    const auto before = t;
    TrafficCoordinator c(g, 1);
    const std::vector requests = {request(1, 0, 2), request(2, 1, 3)};
    CoordinationLimits limits;

    SECTION("attempt budget") { limits.max_attempts = 1; }
    SECTION("search budget") { limits.max_expansions = 1; }
    SECTION("total budget across successful searches") {
        limits.max_expansions = 2;
    }

    auto result = c.replan(t, 0, requests, limits);

    CHECK(result.status == CoordinationStatus::BudgetExhausted);
    CHECK(result.expanded <= limits.max_expansions);
    CHECK(result.plans.empty());
    same(t, before);
}

TEST_CASE("coordinator: stationary outsiders remain obstacles", "[traffic-coordinator]") {
    auto g = graph(3);
    edge(g, 0, 1); edge(g, 1, 2);
    ReservationTable t{ResourceMap{g}};
    hold(t, 1, 0); hold(t, 99, 1);
    const auto before = t;
    TrafficCoordinator c(g, 1);
    const std::vector requests = {request(1, 0, 2)};

    auto result = c.replan(t, 0, requests);

    CHECK(result.status == CoordinationStatus::NoPlanFound);
    same(t, before);
}

TEST_CASE("coordinator: SIPP finds a detour around outsider", "[traffic-coordinator]") {
    auto g = graph(4);
    edge(g, 0, 1); edge(g, 1, 2);
    edge(g, 0, 3); edge(g, 3, 2);
    ReservationTable t{ResourceMap{g}};
    hold(t, 99, 1);
    TrafficCoordinator c(g, 1);
    const std::vector requests = {request(1, 0, 2)};

    auto result = c.replan(t, 0, requests);

    REQUIRE(result.status == CoordinationStatus::Accepted);
    REQUIRE(result.plans[0].path.visits.size() == 3);
    CHECK(result.plans[0].path.visits[1].node == 3);
    CHECK(t.safe_intervals(
        t.resources().node_resource(1), {0, kForever}).empty());
}

TEST_CASE("coordinator: conflicting start never changes table", "[traffic-coordinator]") {
    auto g = crossing();
    ReservationTable t{ResourceMap{g}};
    std::vector requests = {request(1, 0, 3), request(2, 1, 4)};

    SECTION("same start") { requests[1].from = 0; }
    SECTION("external robot occupies start") { hold(t, 99, 0); }
    SECTION("external forecast later occupies start") {
        const std::vector<TimedReservation> p = {{0, {5000, 6000}}};
        REQUIRE_FALSE(t.try_replace(99, p));
    }

    const auto before = t;
    TrafficCoordinator c(g, 1);
    auto result = c.replan(t, 0, requests);

    CHECK(result.status == CoordinationStatus::StartConflict);
    CHECK(result.conflict.has_value());
    CHECK(result.attempts == 0);
    same(t, before);
}

TEST_CASE("coordinator: validates all requests before changing table", "[traffic-coordinator]") {
    auto g = crossing();
    ReservationTable t{ResourceMap{g}};
    hold(t, 1, 0);
    const auto before = t;
    TrafficCoordinator c(g, 1);
    std::vector requests = {request(1, 0, 3), request(2, 1, 4)};
    CoordinationLimits limits;

    SECTION("duplicate robot") { requests[1].robot = 1; }
    SECTION("invalid priority") { requests[1].priority = 4; }
    SECTION("future waiting timestamp") { requests[1].waiting_since = 1; }
    SECTION("negative waiting timestamp") { requests[1].waiting_since = -1; }
    SECTION("missing goal") { requests[1].goals.clear(); }
    SECTION("invalid deadline") { requests[1].latest_arrival = kForever; }
    SECTION("invalid aging period") { limits.aging_period = 0; }
    SECTION("zero attempts") { limits.max_attempts = 0; }
    SECTION("zero expansions") { limits.max_expansions = 0; }

    CHECK_THROWS_AS(
        c.replan(t, 0, requests, limits), std::invalid_argument);
    same(t, before);
}

TEST_CASE("coordinator: map and node validation", "[traffic-coordinator]") {
    auto g = crossing();
    TrafficCoordinator c(g, 1);
    std::vector requests = {request(1, 0, 3)};

    SECTION("map version") {
        ++g.version;
        ReservationTable t{ResourceMap{g}};
        CHECK_THROWS_AS(
            c.replan(t, 0, requests), std::invalid_argument);
    }
    SECTION("start outside graph") {
        ReservationTable t{ResourceMap{g}};
        requests[0].from = 100;
        CHECK_THROWS_AS(
            c.replan(t, 0, requests), std::out_of_range);
    }
    SECTION("goal outside graph") {
        ReservationTable t{ResourceMap{g}};
        requests[0].goals = {100};
        CHECK_THROWS_AS(
            c.replan(t, 0, requests), std::out_of_range);
    }
}

TEST_CASE("coordinator: empty group and extreme valid time", "[traffic-coordinator]") {
    auto g = graph(1);
    ReservationTable t{ResourceMap{g}};
    TrafficCoordinator c(g, 1);

    SECTION("empty group") {
        auto result = c.replan(t, 0, {});
        CHECK(result.status == CoordinationStatus::Accepted);
        CHECK(result.expanded == 0);
    }
    SECTION("aging score does not overflow") {
        auto r = request(1, 0, 0);
        r.priority = 3;
        r.latest_arrival = kForever - 1;
        const std::vector requests = {r};
        CoordinationLimits limits;
        limits.aging_period = 1;

        auto result = c.replan(t, kForever - 1, requests, limits);

        REQUIRE(result.status == CoordinationStatus::Accepted);
        CHECK(result.plans[0].path.visits.front().arrival == kForever - 1);
    }
}