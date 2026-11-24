#include <catch2/catch_test_macros.hpp>
#include "traffic/FixedRouteGate.hpp"

namespace {
using namespace traffic;

enum Node : std::uint32_t { B, C, E, F, X };
enum Edge : std::uint32_t { BC, CB, EF, FC, FB, BX, XC, CX };

struct Stand {
    Graph graph;
    ProtectedRoute issued{"A", "trip/1/s0", "map", 7, {B, C}, {BC}, 0};
    ProtectedRoute origin{"D", "next/1/s0", "map", 7, {E}, {}, 0};
    ProtectedRoute wanted{"D", "next/1/s0", "map", 7, {E, F}, {EF}, 0};
    std::vector<TrafficOccupant> robots{
        {"A", "map", "", false, true}, {"D", "map", "E", true, true}};
    std::vector<ProtectedRoute> held{issued, origin};
    RecoveryBounds bounds{"map", 7, true, {RecoveryMission{{issued}}}};

    Stand() {
        graph.map_id = "map";
        graph.version = 7;
        graph.nodes = {{"B"}, {"C"}, {"E"}, {"F"}, {"X"}};
        graph.node_index = {{"B", B}, {"C", C}, {"E", E}, {"F", F}, {"X", X}};
        graph.edges = {{"BC", B, C, 1}, {"CB", C, B, 1}, {"EF", E, F, 1},
                       {"FC", F, C, 1}, {"FB", F, B, 1}, {"BX", B, X, 1},
                       {"XC", X, C, 1}, {"CX", C, X, 1}};
    }

    bool allowed() const {
        return FixedRouteGate{}.check(graph, robots, held, wanted, bounds).granted;
    }

    void append(Node node, Edge edge) {
        wanted.nodes.push_back(node);
        wanted.edges.push_back(edge);
    }
};
} // namespace

TEST_CASE("known B-C permits E-F during recovery", "[recovery-admission]") {
    Stand s;
    REQUIRE(s.allowed());

    SECTION("shared node") { s.append(C, FC); }
    SECTION("opposing movement") { s.append(C, FC); s.append(B, CB); }
    SECTION("recovering robot requests more base") {
        s.wanted = s.issued;
        s.append(X, CX);
    }
    CHECK_FALSE(s.allowed());
}

TEST_CASE("incomplete or inconsistent coverage stops independent travel", "[recovery-admission]") {
    Stand s;
    auto& base = s.bounds.missions.front().steps.front();
    SECTION("inventory not yet complete") { s.bounds.complete = false; }
    SECTION("another unresolved mission without a bound") { s.bounds.missions.emplace_back(); }
    SECTION("wrong snapshot map") { s.bounds.map = "old"; }
    SECTION("wrong snapshot version") { ++s.bounds.version; }
    SECTION("wrong grant version") { ++base.version; }
    SECTION("another attempt") { base.key = "trip/2/s0"; }
    SECTION("owner missing from inventory") { s.robots.erase(s.robots.begin()); }
    SECTION("owner has no installed route") { s.held.erase(s.held.begin()); }
    SECTION("same owner in two recovery jobs") {
        auto duplicate = s.bounds.missions.front();
        s.bounds.missions.push_back(duplicate);
    }
    SECTION("mixed owners in one job") { s.bounds.missions.front().steps.push_back(s.origin); }
    SECTION("unknown edge") { base.edges.front() = 100; }
    SECTION("wrong edge direction") { base.edges.front() = CB; }
    SECTION("stale progress dropped B and its lane") {
        s.held.front().reached = 1;
        base.reached = 1;
    }
    CHECK_FALSE(s.allowed());
}

TEST_CASE("coverage includes the lanes of every possible step", "[recovery-admission]") {
    Stand s;
    auto& held = s.held.front();
    held.nodes = {B, X, C};
    held.edges = {BX, XC};
    auto& steps = s.bounds.missions.front().steps;
    steps.front() = held;
    REQUIRE(s.allowed());

    // C and B are held, but the direct lane between them is free.
    steps.push_back({"A", "trip/1/s1", "map", 7, {C, B}, {CB}, 0});
    CHECK_FALSE(s.allowed());
    // C-X and X-C use the same physical lane.
    steps.back().nodes = {C, X};
    steps.back().edges = {CX};
    CHECK(s.allowed());
}

TEST_CASE("confirmed stop releases B but still protects C", "[recovery-admission]") {
    Stand s;
    s.append(B, FB);
    REQUIRE_FALSE(s.allowed());

    s.bounds.missions.clear();
    s.held.erase(s.held.begin());
    s.robots.front() = {"A", "map", "C", true, false};
    CHECK(s.allowed());
    s.append(C, BC);
    CHECK_FALSE(s.allowed());
}