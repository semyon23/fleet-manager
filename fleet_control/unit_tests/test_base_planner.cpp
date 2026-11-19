#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "traffic/BasePlanner.hpp"

namespace {

using namespace traffic;

Graph line() {
    Graph g;
    g.map_id = "base-planner-test";
    g.version = 1;

    for (std::uint32_t i = 0; i < 5; ++i) {
        GraphNode node;
        node.id = std::to_string(i);
        g.nodes.push_back(node);

        if (i != 0) {
            GraphEdge edge;
            edge.id = std::to_string(i - 1);
            edge.from = i - 1;
            edge.to = i;
            edge.length = 1;
            g.edges.push_back(edge);
        }
    }

    return g;
}

struct Fixture {
    Graph graph = line();
    BaseGuard guard{graph};
    ReservationTable table{ResourceMap{graph}};
    BasePlanner planner{graph, 1};
    BasePlanRequest request;

    Fixture() {
        REQUIRE_FALSE(guard.begin(1, 10, 0));
        request.robot = 1;
        request.epoch = 10;
        request.goals = {4};
        request.latest_arrival = 20'000;
    }

    void base(
        std::vector<std::uint32_t> nodes,
        std::vector<std::uint32_t> edges) {

        REQUIRE_FALSE(guard.extend(1, 10, nodes, edges));
    }

    void block(ResourceId resource, Interval interval) {
        const std::vector<TimedReservation> plan = {
            {resource, interval}
        };
        REQUIRE_FALSE(table.try_replace(99, plan));
    }

    BasePlanResult update() {
        return planner.update(guard, table, request);
    }
};

void same(const ReservationTable& a, const ReservationTable& b) {
    for (ResourceId id = 0; id < a.resources().size(); ++id) {
        CHECK(
            a.safe_intervals(id, {0, kForever}) ==
            b.safe_intervals(id, {0, kForever}));

        CHECK(
            a.safe_intervals(id, {0, kForever}, 1) ==
            b.safe_intervals(id, {0, kForever}, 1));
    }
}

} // namespace

TEST_CASE(
    "base planner: extends ahead of arrival and keeps base forecast",
    "[traffic-base-plan]") {

    Fixture f;
    f.base({0, 1, 2}, {0, 1});

    auto result = f.update();

    REQUIRE(result.status == BasePlanStatus::Extended);
    CHECK(result.boundary_arrival == 2000);
    CHECK(result.nodes == std::vector<std::uint32_t>{0, 1, 2, 3, 4});
    CHECK(f.guard.snapshot(1).reached == 0);
    CHECK(f.table.safe_intervals(
        f.table.resources().lane_resource(0), {0, 1000}).empty());
    CHECK(f.table.safe_intervals(
        f.table.resources().node_resource(4), {4000, kForever}).empty());
    CHECK(f.guard.owner(f.guard.resources().node_resource(4)) == 1);
}

TEST_CASE(
    "base planner: remaining ETA starts at last confirmed node",
    "[traffic-base-plan]") {

    Fixture f;
    f.base({0, 1, 2}, {0, 1});
    REQUIRE(f.guard.advance(1, 10, 1));
    f.request.now = 1500;

    auto result = f.update();

    REQUIRE(result.status == BasePlanStatus::Extended);
    CHECK(result.boundary_arrival == 2500);
    CHECK_FALSE(f.guard.owner(f.guard.resources().node_resource(0)));
    CHECK(f.guard.snapshot(1).reached == 1);
}

TEST_CASE(
    "base planner: future wait limits base at waiting node",
    "[traffic-base-plan]") {

    Fixture f;
    f.base({0, 1}, {0});
    f.block(f.table.resources().lane_resource(2), {0, 8000});

    auto result = f.update();

    REQUIRE(result.status == BasePlanStatus::Extended);
    CHECK(result.nodes == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(result.tail.visits[1].departure == 8000);
    CHECK_FALSE(f.guard.owner(f.guard.resources().lane_resource(2)));
    CHECK(f.table.safe_intervals(
        f.table.resources().node_resource(4), {10000, kForever}).empty());
}

TEST_CASE(
    "base planner: wait ends only with time and hard availability",
    "[traffic-base-plan]") {

    Fixture f;
    f.base({0, 1, 2}, {0, 1});
    f.block(f.table.resources().lane_resource(2), {0, 8000});

    auto waiting = f.update();

    REQUIRE(waiting.status == BasePlanStatus::Waiting);
    CHECK(waiting.nodes.empty());
    CHECK(f.guard.snapshot(1).nodes ==
          std::vector<std::uint32_t>{0, 1, 2});

    REQUIRE(f.guard.advance(1, 10, 2));
    f.request.now = 8000;

    SECTION("free resources") {
        auto result = f.update();
        CHECK(result.status == BasePlanStatus::Extended);
    }

    SECTION("robot still occupies next node") {
        REQUIRE_FALSE(f.guard.begin(2, 1, 3));
        const auto before = f.table;

        auto result = f.update();

        REQUIRE(result.status == BasePlanStatus::HardConflict);
        REQUIRE(result.hard_conflict);
        CHECK(result.hard_conflict->owner == 2);
        CHECK(result.nodes.empty());
        CHECK_FALSE(f.guard.owner(f.guard.resources().lane_resource(2)));
        same(f.table, before);
    }
}

TEST_CASE(
    "base planner: changed prefix forecast conflict rolls back",
    "[traffic-base-plan]") {

    Fixture f;
    f.base({0, 1, 2}, {0, 1});

    // Возможная рассинхронизация после задержки: прогноз другого робота
    // пересёк фактически защищённый base. Выданный base нельзя переписать.
    f.block(f.table.resources().lane_resource(0), {0, 1000});
    const auto before = f.table;

    auto result = f.update();

    REQUIRE(result.status == BasePlanStatus::ForecastConflict);
    REQUIRE(result.forecast_conflict);
    CHECK(result.forecast_conflict->owner == 99);
    CHECK(f.guard.snapshot(1).nodes ==
          std::vector<std::uint32_t>{0, 1, 2});
    CHECK(f.guard.owner(f.guard.resources().lane_resource(0)) == 1);
    same(f.table, before);
}

TEST_CASE(
    "base planner: unsuccessful search preserves old forecast",
    "[traffic-base-plan]") {

    Fixture f;
    f.base({0, 1, 2}, {0, 1});
    SippStatus expected = SippStatus::NoPathWithinWindow;

    SECTION("occupied goal") {
        f.block(f.table.resources().node_resource(4), {0, kForever});
    }

    SECTION("occupied boundary") {
        f.block(f.table.resources().node_resource(2), {0, kForever});
        expected = SippStatus::StartBlocked;
    }

    SECTION("expansion budget") {
        f.request.max_expansions = 1;
        expected = SippStatus::ExpansionLimit;
    }

    const auto before = f.table;
    auto result = f.update();

    CHECK(result.status == BasePlanStatus::SearchFailed);
    CHECK(result.tail.status == expected);
    CHECK(f.guard.snapshot(1).nodes ==
          std::vector<std::uint32_t>{0, 1, 2});
    same(f.table, before);
}

TEST_CASE(
    "base planner: already issued goal is not mission completion",
    "[traffic-base-plan]") {

    Fixture f;
    f.base({0, 1, 2}, {0, 1});
    f.request.goals = {2};

    auto result = f.update();

    REQUIRE(result.status == BasePlanStatus::BoundaryIsGoal);
    CHECK(result.boundary_arrival == 2000);
    CHECK(f.guard.snapshot(1).reached == 0);
    CHECK(result.nodes.empty());
    CHECK(f.table.safe_intervals(
        f.table.resources().node_resource(2), {2000, kForever}).empty());
}

TEST_CASE(
    "base planner: time window and overflow preserve state",
    "[traffic-base-plan]") {

    Fixture f;
    f.base({0, 1, 2}, {0, 1});

    SECTION("short window") {
        f.request.latest_arrival = 1000;
    }

    SECTION("near time limit") {
        f.request.now = kForever - 100;
        f.request.latest_arrival = kForever - 1;
    }

    const auto before = f.table;
    auto result = f.update();

    CHECK(result.status == BasePlanStatus::BaseOutsideWindow);
    CHECK(f.guard.snapshot(1).nodes ==
          std::vector<std::uint32_t>{0, 1, 2});
    same(f.table, before);
}

TEST_CASE(
    "base planner: invalid request cannot change state",
    "[traffic-base-plan]") {

    Fixture f;

    SECTION("stale epoch") {
        f.request.epoch = 9;
    }
    SECTION("empty goals") {
        f.request.goals.clear();
    }
    SECTION("negative time") {
        f.request.now = -1;
    }
    SECTION("invalid window") {
        f.request.latest_arrival = kForever;
    }
    SECTION("zero budget") {
        f.request.max_expansions = 0;
    }

    CHECK_THROWS_AS(f.update(), std::invalid_argument);
    CHECK(f.guard.snapshot(1).nodes == std::vector<std::uint32_t>{0});
}

TEST_CASE(
    "base planner: map mismatch is rejected",
    "[traffic-base-plan]") {

    Fixture f;
    auto graph = f.graph;
    ++graph.version;
    ReservationTable other{ResourceMap{graph}};

    CHECK_THROWS_AS(
        f.planner.update(f.guard, other, f.request),
        std::invalid_argument);
}

TEST_CASE(
    "base planner: prefix and SIPP share edge duration",
    "[traffic-base-plan]") {

    auto graph = line();
    for (auto& edge : graph.edges) {
        edge.max_speed = 3;
        edge.cost = 100;
    }

    BaseGuard guard(graph);
    ReservationTable table{ResourceMap{graph}};
    BasePlanner planner(graph, 1);

    REQUIRE_FALSE(guard.begin(1, 10, 0));

    const std::vector<std::uint32_t> nodes = {0, 1};
    const std::vector<std::uint32_t> edges = {0};
    REQUIRE_FALSE(guard.extend(1, 10, nodes, edges));

    BasePlanRequest request;
    request.robot = 1;
    request.epoch = 10;
    request.goals = {2};
    request.latest_arrival = 2000;

    auto result = planner.update(guard, table, request);

    REQUIRE(result.status == BasePlanStatus::Extended);
    CHECK(result.boundary_arrival == 334);
    CHECK(result.tail.visits.back().arrival == 668);
}