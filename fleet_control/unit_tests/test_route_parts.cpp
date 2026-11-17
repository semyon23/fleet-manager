#include <catch2/catch_test_macros.hpp>

#include "missions/RouteParts.hpp"

namespace {

using Strings = std::vector<std::string>;

// a --ab-- b --bc-- c --cd-- d
const Strings nodes = {"a", "b", "c", "d"};
const Strings edges = {"ab", "bc", "cd"};

    fms::v1::MoveProgress progress(
    const Strings& path,
    std::uint32_t at,
    std::uint32_t released) {

    fms::v1::MoveProgress result;
    result.set_last_node_index(at);
    result.set_last_node_id(path.at(at));
    result.set_released_nodes(released);
    result.set_total_nodes(
        static_cast<std::uint32_t>(path.size()));
    return result;
}

}  // namespace

TEST_CASE("route: whole base, passed nodes removed", "[route]") {
    auto result = split_route(nodes, edges, progress(nodes, 1, 4));

    REQUIRE(result);
    CHECK(result->released.nodes == Strings{"b", "c", "d"});
    CHECK(result->released.edges == Strings{"bc", "cd"});
    CHECK(result->planned.nodes.empty());
    CHECK(result->planned.edges.empty());
}

TEST_CASE("route: base and horizon share the stitching node", "[route]") {
    auto result = split_route(nodes, edges, progress(nodes, 0, 2));

    REQUIRE(result);
    CHECK(result->released.nodes == Strings{"a", "b"});
    CHECK(result->released.edges == Strings{"ab"});
    CHECK(result->planned.nodes == Strings{"b", "c", "d"});
    CHECK(result->planned.edges == Strings{"bc", "cd"});
}

TEST_CASE("route: waiting at the end of base", "[route]") {
    auto state = progress(nodes, 1, 2);
    state.set_waiting_for_release(true);

    auto result = split_route(nodes, edges, state);

    REQUIRE(result);
    CHECK(result->released.nodes == Strings{"b"});
    CHECK(result->released.edges.empty());
    CHECK(result->planned.nodes == Strings{"b", "c", "d"});
    CHECK(result->planned.edges == Strings{"bc", "cd"});
}

TEST_CASE("route: only the starting node released", "[route]") {
    auto result = split_route(nodes, edges, progress(nodes, 0, 1));

    REQUIRE(result);
    CHECK(result->released.nodes == Strings{"a"});
    CHECK(result->released.edges.empty());
    CHECK(result->planned.nodes == nodes);
    CHECK(result->planned.edges == edges);
}

TEST_CASE("route: repeated node is identified by index", "[route]") {
    // a --ab-- b --ba-- a --ac-- c
    const Strings loop_nodes = {"a", "b", "a", "c"};
    const Strings loop_edges = {"ab", "ba", "ac"};

    auto result = split_route(
        loop_nodes, loop_edges, progress(loop_nodes, 2, 4));

    REQUIRE(result);
    CHECK(result->released.nodes == Strings{"a", "c"});
    CHECK(result->released.edges == Strings{"ac"});
    CHECK(result->planned.nodes.empty());
}

TEST_CASE("route: action at destination has no line", "[route]") {
    auto result = split_route(nodes, edges, progress(nodes, 3, 4));

    REQUIRE(result);
    CHECK(result->released.nodes == Strings{"d"});
    CHECK(result->released.edges.empty());
    CHECK(result->planned.nodes.empty());
}

TEST_CASE("route: single-node step", "[route]") {
    const Strings single = {"a"};
    const Strings no_edges;

    auto result = split_route(
        single, no_edges, progress(single, 0, 1));

    REQUIRE(result);
    CHECK(result->released.nodes == single);
    CHECK(result->released.edges.empty());
    CHECK(result->planned.nodes.empty());
}

TEST_CASE("route: invalid progress does not produce a path", "[route]") {
    auto state = progress(nodes, 0, 4);

    SECTION("empty base") {
        state.set_released_nodes(0);
    }

    SECTION("base exceeds route") {
        state.set_released_nodes(5);
    }

    SECTION("position outside base") {
        state.set_last_node_index(2);
        state.set_last_node_id("c");
        state.set_released_nodes(2);
    }

    SECTION("different route size") {
        state.set_total_nodes(5);
    }

    SECTION("node and index disagree") {
        state.set_last_node_id("d");
    }

    CHECK_FALSE(split_route(nodes, edges, state));
}

TEST_CASE("route: malformed geometry is rejected", "[route]") {
    auto state = progress(nodes, 0, 4);

    SECTION("missing edge") {
        const Strings incomplete = {"ab", "bc"};
        CHECK_FALSE(split_route(nodes, incomplete, state));
    }

    SECTION("empty route") {
        const Strings empty;
        CHECK_FALSE(split_route(empty, empty, state));
    }
}