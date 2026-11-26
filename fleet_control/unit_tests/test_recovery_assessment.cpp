#include <catch2/catch_test_macros.hpp>
#include "missions/RecoveryAssessment.hpp"

namespace {
using namespace traffic;
using namespace recovery_assessment;
using Inventory = fms::dispatch::v1::GetRecoveryInventoryResponse;

struct Stand {
    Graph graph;
    Inventory inv;
    ProtectedRoute issued{"A","trip/0/s0","map",1,{0,1},{0},0};
    ProtectedRoute wanted{"D","next/0/s0","map",1,{2,3},{1},0};
    std::vector<TrafficOccupant> fleet{{"A","map","",false,true}, {"D","map","E",true,false}};
    std::vector<ProtectedRoute> held{issued};
    Local local{{"trip", {"A",0,{issued},true}}};
    std::string extend;
    bool fresh = true;

    Stand() {
        graph.map_id = "map";
        graph.version = 1;
        graph.nodes = {{"B"},{"C"},{"E"},{"F"}};
        graph.node_index = {{"B",0},{"C",1},{"E",2},{"F",3}};
        graph.edges = {{"BC",0,1,1},{"EF",2,3,1},{"FC",3,1,1},{"CB",1,0,1}};
        inv.set_state(Inventory::READY);
        inv.set_instance_id("run");
        inv.set_revision(1);
        inv.add_robots("A");
        inv.add_robots("D");
        inv.mutable_live_restrictions();
        record("trip", "A");
    }
    void record(const std::string& id, const std::string& robot) {
        auto* row = inv.add_missions();
        row->set_robot_id(robot);
        row->set_mission_id(id);
        row->set_add_attempt(0);
        inv.add_blocked_robots(robot);
    }
    bool allowed() const {
        return assess(graph, inv, fleet, held, local, wanted, extend, fresh).granted;
    }
};
} // namespace

TEST_CASE("Preview relates the whole fleet to issued bases", "[recovery-assessment]") {
    Stand s;
    REQUIRE(s.allowed());
    SECTION("approach to occupied C and head-on C-B") {
        s.wanted.nodes = {2,3,1,0};
        s.wanted.edges = {1,2,3};
    }
    SECTION("missing second step protection") {
        s.local.at("trip").steps.push_back({"A","trip/0/s1","map",1,{3,1},{2},0});
    }
    SECTION("job awaiting its journal") { s.local.at("trip").steps.clear(); }
    SECTION("locally closed document still has an open job") {
        s.inv.clear_missions();
        s.inv.clear_blocked_robots();
        CHECK(s.allowed());
        s.held.clear();
    }
    SECTION("unaccounted dispatch mission") { s.local.clear(); }
    SECTION("old attempt") { s.inv.mutable_missions(0)->set_add_attempt(1); }
    SECTION("unknown attempt is different from zero") { s.inv.mutable_missions(0)->clear_add_attempt(); }
    SECTION("same fleet size different identities") { s.inv.set_robots(0,"other"); }
    SECTION("unavailable live owner needs its whole possible base protected") {
        s.local.at("trip").recovering = false;
        s.held.front().reached = 1;
        CHECK(s.allowed());
        s.inv.mutable_live_restrictions()->add_robots("A");
    }
    SECTION("not ready") { s.inv.set_state(Inventory::CONFLICT); }
    SECTION("blocked robot with no bounded mission") { s.inv.add_blocked_robots("D"); }
    SECTION("unknown position on another map") {
        s.inv.add_robots("X");
        s.fleet.push_back({"X","other","",false,false});
    }
    SECTION("malformed live restriction") { s.inv.mutable_live_restrictions()->add_robots("D"); }
    SECTION("fresh observation lost") { s.fresh = false; }
    SECTION("target is not stopped at the origin") { s.fleet.back().node = "F"; }
    CHECK_FALSE(s.allowed());
}

TEST_CASE("Only the verified owner can preview its extension", "[recovery-assessment]") {
    Stand s;
    s.extend = "next";
    s.record("next","D");
    s.fleet.back().has_mission = true;
    s.held.push_back({"D","next/0/s0","map",1,{2},{},0});
    s.local.emplace("next", LocalEvidence{"D",0,{s.held.back()},false});
    REQUIRE(s.allowed());
    SECTION("Add cannot use a busy target") { s.extend.clear(); }
    SECTION("different live step") { s.wanted.key = "next/0/s1"; }
    SECTION("pending command or unusable live state") {
        s.inv.mutable_live_restrictions()->add_robots("D");
    }
    SECTION("older dispatch lacks distinct live restrictions") { s.inv.clear_live_restrictions(); }
    SECTION("uncertain outcome") { s.inv.mutable_missions(1)->set_uncertain(true); }
    SECTION("target itself is recovering") { s.local.at("next").recovering = true; }
    SECTION("supposedly live mission disappeared") { s.inv.mutable_missions()->RemoveLast(); }
    CHECK_FALSE(s.allowed());
}

TEST_CASE("The live robot must confirm the current MoveStep", "[recovery-assessment]") {
    namespace fms1 = fms::v1;
    fms1::RobotStatus robot;
    robot.set_mission_id("trip");
    robot.set_connection(fms1::CONNECTION_STATE_ONLINE);
    robot.set_operating_mode(fms1::OPERATING_MODE_AUTOMATIC);
    robot.mutable_safety()->set_emergency_stop(fms1::EMERGENCY_STOP_NONE);
    robot.set_last_node_id("B");
    auto* order = robot.mutable_observed_order();
    order->set_order_id("trip-s2");
    order->set_node_sequence(4);
    order->set_update_id(1);
    fms1::MoveProgress progress;
    progress.set_last_node_id("B");
    progress.set_last_node_index(2);
    progress.set_confirmed_revision(1);
    auto matches = [&] { return matches_move(robot, "trip", 2, progress); };
    REQUIRE(matches());
    SECTION("foreign mission") { robot.set_mission_id("other"); }
    SECTION("old step or retry") { order->set_order_id("trip-s2-a2"); }
    SECTION("old update") { order->set_update_id(0); }
    SECTION("repeated node at a different index") { order->set_node_sequence(0); }
    SECTION("odd sequence ID") { order->set_node_sequence(5); }
    SECTION("idle while a horizon remains") { order->set_idle(true); }
    SECTION("safety stop") { robot.mutable_safety()->set_emergency_stop(fms1::EMERGENCY_STOP_REMOTE); }
    CHECK_FALSE(matches());
}

TEST_CASE("An extension rechecks the reserved destination after persistence", "[recovery-send]") {
    Stand s;
    s.record("next", "D");
    s.extend = "next";
    s.fleet.back().has_mission = true;
    s.inv.mutable_live_restrictions()->add_robots("A");
    const ProtectedRoute origin{"D", "next/0/s0", "map", 1, {2}, {}, 0};
    s.held.push_back(origin);
    s.local.emplace("next", LocalEvidence{"D",0,{origin},false});
    REQUIRE(s.allowed());

    s.held.back() = s.wanted;
    s.local.at("next").steps = {s.wanted};
    REQUIRE(s.allowed());
    SECTION("D loses its live session") { s.inv.mutable_live_restrictions()->add_robots("D"); }
    SECTION("D no longer confirms this step") { s.local.at("next").recovering = true; }
    SECTION("D's durable identity changed") { s.inv.mutable_missions(1)->set_add_attempt(1); }
    SECTION("A gains an unaccounted grant to F") {
        s.local.at("trip").steps.push_back({"A", "trip/0/s1", "map", 1, {3}, {}, 0});
    }
    CHECK_FALSE(s.allowed());
}

TEST_CASE("The pending Add must still own only its origin", "[recovery-send]") {
    Stand s;
    REQUIRE(s.allowed());
    s.held.push_back({"D", "next/0/s0", "map", 1, {2}, {}, 0});
    auto recheck = [&] {
        return assess(s.graph, s.inv, s.fleet, s.held, s.local, s.wanted, "", true, nullptr, true).granted;
    };
    REQUIRE(recheck());

    SECTION("another local reservation owns E") { s.held.back().key = "other/0/s0"; }
    SECTION("earlier Add attempt owns E") { s.held.back().key = "next/1/s0"; }
    SECTION("reservation already extends beyond the origin") { s.held.back() = s.wanted; }
    SECTION("map was replaced") { ++s.held.back().version; }
    SECTION("origin reservation disappeared") { s.held.pop_back(); }
    SECTION("robot left E while storage was writing") { s.fleet.back().node = "F"; }
    SECTION("dispatch already owns D") { s.record("next", "D"); }
    SECTION("A's possible grant reaches F") {
        s.local.at("trip").steps.push_back({"A", "trip/0/s1", "map", 1, {3}, {}, 0});
    }
    SECTION("the path now crosses C") {
        s.wanted.nodes = {2,3,1};
        s.wanted.edges = {1,2};
    }
    CHECK_FALSE(recheck());
}
