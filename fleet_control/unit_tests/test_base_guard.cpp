#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <vector>

#include "traffic/BaseGuard.hpp"

namespace {

using namespace traffic;

Graph map() {
    Graph g;
    g.map_id = "base-test";
    g.version = 1;

    for (int i = 0; i < 4; ++i) {
        GraphNode n;
        n.id = std::to_string(i);
        g.nodes.push_back(n);
    }

    for (auto [a, b] : {
            std::pair{0, 1}, {1, 2}, {2, 3}, {1, 0}, {0, 2}}) {
        GraphEdge e;
        e.id = std::to_string(g.edges.size());
        e.from = a;
        e.to = b;
        e.length = 1;
        g.edges.push_back(e);
    }

    return g;
}

std::optional<Conflict> extend(
    BaseGuard& b,
    std::vector<std::uint32_t> n,
    std::vector<std::uint32_t> e) {

    return b.extend(1, 10, n, e);
}

} // namespace

TEST_CASE(
    "base: reserves before send and releases on arrival",
    "[traffic-base]") {

    BaseGuard b(map());
    REQUIRE_FALSE(b.begin(1, 10, 0));
    REQUIRE_FALSE(extend(b, {0, 1, 2}, {0, 1}));

    CHECK(b.owner(b.resources().node_resource(2)) == 1);
    CHECK(b.owner(b.resources().lane_resource(1)) == 1);

    REQUIRE(b.advance(1, 10, 1));

    CHECK_FALSE(b.owner(b.resources().node_resource(0)));
    CHECK_FALSE(b.owner(b.resources().lane_resource(0)));
    CHECK(b.owner(b.resources().node_resource(1)) == 1);
    CHECK(b.owner(b.resources().lane_resource(1)) == 1);

    REQUIRE(b.advance(1, 10, 2));

    CHECK_FALSE(b.owner(b.resources().node_resource(1)));
    CHECK_FALSE(b.owner(b.resources().lane_resource(1)));
    CHECK(b.owner(b.resources().node_resource(2)) == 1);
}

TEST_CASE(
    "base: extension while moving preserves progress",
    "[traffic-base]") {

    BaseGuard b(map());
    REQUIRE_FALSE(b.begin(1, 10, 0));
    REQUIRE_FALSE(extend(b, {0, 1, 2}, {0, 1}));
    REQUIRE(b.advance(1, 10, 1));

    REQUIRE_FALSE(extend(b, {0, 1, 2, 3}, {0, 1, 2}));

    CHECK(b.snapshot(1).reached == 1);
    CHECK(b.snapshot(1).nodes.back() == 3);
    CHECK_FALSE(b.owner(b.resources().node_resource(0)));
    CHECK(b.owner(b.resources().node_resource(3)) == 1);
}

TEST_CASE("base: conflict is atomic", "[traffic-base]") {
    BaseGuard b(map());
    REQUIRE_FALSE(b.begin(1, 10, 0));
    REQUIRE_FALSE(b.begin(2, 20, 2));

    auto conflict = extend(b, {0, 1, 2}, {0, 1});

    REQUIRE(conflict);
    CHECK(conflict->owner == 2);
    CHECK(b.snapshot(1).nodes == std::vector<std::uint32_t>{0});
    CHECK_FALSE(b.owner(b.resources().node_resource(1)));
    CHECK_FALSE(b.owner(b.resources().lane_resource(0)));
    CHECK(b.owner(b.resources().node_resource(0)) == 1);
    CHECK(b.owner(b.resources().node_resource(2)) == 2);

    REQUIRE(b.begin(3, 30, 2));
    CHECK_THROWS_AS(b.snapshot(3), std::out_of_range);
}

TEST_CASE(
    "base: cannot rewrite or shrink issued prefix",
    "[traffic-base]") {

    BaseGuard b(map());
    REQUIRE_FALSE(b.begin(1, 10, 0));
    REQUIRE_FALSE(extend(b, {0, 1, 2}, {0, 1}));

    SECTION("shrink") {
        CHECK_THROWS_AS(
            extend(b, {0, 1}, {0}), std::invalid_argument);
    }
    SECTION("reroute") {
        CHECK_THROWS_AS(
            extend(b, {0, 2, 3}, {4, 2}), std::invalid_argument);
    }
    SECTION("new epoch before arrival") {
        CHECK_THROWS_AS(b.begin(1, 11, 2), std::invalid_argument);
    }

    CHECK(b.snapshot(1).nodes == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(b.owner(b.resources().lane_resource(0)) == 1);
}

TEST_CASE(
    "base: repeated nodes and opposing lanes stay protected",
    "[traffic-base]") {

    BaseGuard b(map());
    REQUIRE_FALSE(b.begin(1, 10, 0));
    REQUIRE_FALSE(extend(b, {0, 1, 0, 2}, {0, 3, 4}));

    REQUIRE(b.advance(1, 10, 1));

    CHECK(b.owner(b.resources().node_resource(0)) == 1);
    CHECK(b.owner(b.resources().lane_resource(0)) == 1);

    REQUIRE(b.advance(1, 10, 2));

    CHECK_FALSE(b.owner(b.resources().lane_resource(0)));
    CHECK_FALSE(b.owner(b.resources().node_resource(1)));
    CHECK(b.owner(b.resources().node_resource(0)) == 1);
}

TEST_CASE(
    "base: stale progress and duplicate requests",
    "[traffic-base]") {

    BaseGuard b(map());
    REQUIRE_FALSE(b.begin(1, 10, 0));
    REQUIRE_FALSE(extend(b, {0, 1, 2}, {0, 1}));
    REQUIRE(b.advance(1, 10, 1));

    CHECK_FALSE(b.advance(1, 9, 2));
    CHECK_FALSE(b.advance(1, 11, 2));
    CHECK_FALSE(b.advance(1, 10, 0));
    CHECK(b.advance(1, 10, 1));
    CHECK_FALSE(b.begin(1, 10, 0));
    CHECK_FALSE(extend(b, {0, 1, 2}, {0, 1}));

    CHECK(b.snapshot(1).reached == 1);
    CHECK_FALSE(b.owner(b.resources().node_resource(0)));
    CHECK_THROWS_AS(b.advance(1, 10, 3), std::out_of_range);
    CHECK(b.owner(b.resources().node_resource(1)) == 1);
}

TEST_CASE(
    "base: next route retains final occupancy and rejects old epoch",
    "[traffic-base]") {

    BaseGuard b(map());
    REQUIRE_FALSE(b.begin(1, 10, 0));
    REQUIRE_FALSE(extend(b, {0, 1, 2}, {0, 1}));
    REQUIRE(b.advance(1, 10, 2));

    CHECK_THROWS_AS(b.begin(1, 11, 3), std::invalid_argument);
    REQUIRE_FALSE(b.begin(1, 11, 2));

    CHECK(b.snapshot(1).nodes == std::vector<std::uint32_t>{2});
    CHECK(b.owner(b.resources().node_resource(2)) == 1);
    CHECK_FALSE(b.advance(1, 10, 2));
    CHECK_THROWS_AS(b.begin(1, 10, 0), std::invalid_argument);

    REQUIRE_FALSE(b.begin(2, 1, 0));
}

TEST_CASE(
    "base: malformed extension keeps ownership",
    "[traffic-base]") {

    BaseGuard b(map());
    REQUIRE_FALSE(b.begin(1, 10, 0));

    SECTION("wrong topology") {
        CHECK_THROWS_AS(
            extend(b, {0, 2}, {0}), std::invalid_argument);
    }
    SECTION("bad edge") {
        CHECK_THROWS_AS(
            extend(b, {0, 1}, {99}), std::out_of_range);
    }
    SECTION("bad node") {
        CHECK_THROWS_AS(
            extend(b, {0, 99}, {0}), std::out_of_range);
    }
    SECTION("bad shape") {
        CHECK_THROWS_AS(
            extend(b, {0, 1}, {}), std::invalid_argument);
    }

    CHECK(b.snapshot(1).nodes == std::vector<std::uint32_t>{0});
    CHECK(b.owner(b.resources().node_resource(0)) == 1);
    CHECK_FALSE(b.owner(b.resources().node_resource(1)));
}