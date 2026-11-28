#include "ExecutionRecoveryFixture.hpp"

#include <map>

using recovery_test::Trip;
using execution_recovery::Checkpoint;
namespace db = fms::database::v1;
namespace t = fms::v1;
namespace dp = fms::dispatch::v1;

TEST_CASE("Finishing the last checkpoint wakes assignment once", "[mission-queue-wakeup]") {
    std::map<std::string, int> pending{{"trip-a", 1}, {"trip-b", 1}};
    int wakes = 0;
    const auto wake = [&] {
        // Назначение может проверить очередь прямо из обработчика сигнала.
        CHECK(pending.empty());
        ++wakes;
    };

    execution_recovery::finish_checkpoint_job(pending, "missing", wake);
    CHECK(wakes == 0);
    CHECK(pending.size() == 2);

    execution_recovery::finish_checkpoint_job(pending, "trip-a", wake);
    CHECK(wakes == 0);
    CHECK(pending.contains("trip-b"));

    execution_recovery::finish_checkpoint_job(pending, "trip-b", wake);
    CHECK(wakes == 1);

    // Повтор последнего подтверждения не порождает второй сигнал.
    execution_recovery::finish_checkpoint_job(pending, "trip-b", wake);
    CHECK(wakes == 1);
}

TEST_CASE("Only the matching Add receipt and effective route can finish a mission", "[execution-recovery]") {
    Trip trip;
    SECTION("confirmed completion") {
        const auto d = trip.decide();
        REQUIRE(d.terminal);
        REQUIRE(d.held);
        CHECK(d.held->nodes == trip.nodes);
        CHECK(d.held->edges == trip.edges);
        CHECK(d.stopped_on_node(trip.graph, &trip.robot));
        return;
    }
    SECTION("zero attempt must be present") { trip.cp.clear_add_attempt(); }
    SECTION("old attempt") { trip.cp.set_add_attempt(1); }
    SECTION("wrong robot") { trip.cp.mutable_status()->set_robot_id("other"); }
    SECTION("wrong mission") { trip.cp.set_mission_id("other"); }
    SECTION("unversioned") { trip.cp.set_checkpoint_version(0); }
    SECTION("no terminal checkpoint") { trip.cp.set_kind(Checkpoint::ACTIVE); }
    SECTION("uncertain") { trip.cp.set_kind(Checkpoint::UNCERTAIN); }
    SECTION("different immutable Add") { trip.cp.set_accepted_add("other"); }
    SECTION("same IDs but different physical route") {
        trip.cp.mutable_executed_mission()->mutable_nodes(0)->mutable_move()->mutable_nodes(1)->set_x(1);
    }
    SECTION("progress beyond the grant") {
        trip.cp.mutable_status()->mutable_steps(0)->mutable_move()->set_last_node_index(2);
    }
    CHECK_FALSE(trip.decide().terminal);
}

TEST_CASE("Journal revisions and protected graph indices must agree", "[execution-recovery]") {
    Trip trip;
    SECTION("different map version") { trip.journal[1].set_map_version(8); }
    SECTION("changed request in the control document") { trip.mission.requested_robot_id = "other"; }
    SECTION("newer Add attempt") { trip.journal[0].set_attempt(1); }
    SECTION("extension missing") {
        trip.journal.pop_back();
        CHECK_FALSE(trip.decide().terminal);
        return;
    }
    SECTION("revision gap") { trip.journal[1].set_revision(2); }
    SECTION("duplicate extension") { auto copy = trip.journal[1]; trip.journal.push_back(copy); }
    SECTION("wrong edge in snapshot") {
        trip.snapshot["protectedBase"]["edges"] = {1};
        trip.journal[1].set_recovery_json(trip.snapshot.dump());
    }
    SECTION("edge in reverse direction") { trip.graph.edges[0].from = 1; }
    SECTION("corrupt bytes") { trip.journal[0].set_request_proto("bad"); }
    CHECK_FALSE(trip.authority().has_value());
}

TEST_CASE("A finished document does not by itself release the approach to C", "[execution-recovery]") {
    Trip trip;
    SECTION("cached pose") { trip.robot.clear_observed_order(); }
    SECTION("braking in C") { trip.robot.set_driving(true); }
    SECTION("offline") { trip.robot.set_connection(t::CONNECTION_STATE_OFFLINE); }
    SECTION("not localized") { trip.robot.set_localized(false); }
    SECTION("other map") { trip.robot.mutable_pose()->set_map_id("old"); }
    SECTION("between B and C") { trip.robot.mutable_pose()->set_x(4); }
    SECTION("foreign order") { trip.robot.mutable_observed_order()->set_order_id("other"); }
    SECTION("new mission") { trip.robot.set_mission_id("other"); }
    SECTION("wrong update") { trip.robot.mutable_observed_order()->set_update_id(0); }
    SECTION("wrong absolute index") { trip.robot.mutable_observed_order()->set_node_sequence(0); }
    SECTION("unfinished actions") { trip.robot.mutable_observed_order()->set_idle(false); }
    SECTION("missing last grant") { trip.cp.clear_grant_update(); }
    SECTION("extension was not confirmed") {
        trip.cp.mutable_status()->mutable_steps(0)->mutable_move()->set_confirmed_revision(0);
    }
    SECTION("destination was skipped") {
        trip.policy(t::FAILURE_ACTION_SKIP);
        trip.cp.mutable_status()->mutable_steps(0)->set_state(t::STEP_STATE_SKIPPED);
    }
    const auto d = trip.decide();
    REQUIRE(d.terminal);
    REQUIRE(d.held);
    CHECK(d.held->nodes == trip.nodes);
    CHECK(d.held->edges == trip.edges);
    CHECK_FALSE(d.stopped_on_node(trip.graph, &trip.robot));
    CHECK_FALSE(d.stopped_on_node(trip.graph, nullptr));
}

TEST_CASE("Cancel mid-edge keeps B-C even if lastNodeId still says B", "[execution-recovery]") {
    Trip trip;
    auto* status = trip.cp.mutable_status();
    status->set_state(t::MISSION_STATE_CANCELED);
    auto* step = status->mutable_steps(0);
    step->set_state(t::STEP_STATE_CANCELED);
    CHECK_FALSE(trip.decide().terminal);
    dp::CancelMissionRequest cancel;
    cancel.set_mission_id("trip");
    trip.record(db::CONTROL_COMMAND_KIND_CANCEL_MISSION, cancel.SerializeAsString());
    CHECK(trip.decide().stopped_on_node(trip.graph, &trip.robot));
    step->mutable_move()->set_last_node_id("B");
    step->mutable_move()->set_last_node_index(0);
    trip.robot.set_last_node_id("B");
    trip.robot.mutable_observed_order()->set_node_sequence(0);
    trip.robot.mutable_pose()->set_x(0.1);
    const auto d = trip.decide();
    REQUIRE(d.terminal);
    REQUIRE(d.held);
    CHECK(d.held->edges == trip.edges);
    CHECK_FALSE(d.stopped_on_node(trip.graph, &trip.robot));
}

TEST_CASE("Repeated node and order retry have distinct physical identities", "[execution-recovery]") {
    Trip trip(true);
    trip.policy(t::FAILURE_ACTION_RETRY);
    auto* step = trip.cp.mutable_status()->mutable_steps(0);
    step->set_attempts(2);
    trip.cp.set_grant_order("trip-s0-a2");
    const auto d = trip.decide();
    REQUIRE(d.terminal);
    CHECK_FALSE(d.stopped_on_node(trip.graph, &trip.robot));
    trip.robot.mutable_observed_order()->set_order_id("trip-s0-a2");
    trip.robot.mutable_observed_order()->set_node_sequence(0);
    CHECK_FALSE(d.stopped_on_node(trip.graph, &trip.robot));
    trip.robot.mutable_observed_order()->set_node_sequence(4);
    CHECK(d.stopped_on_node(trip.graph, &trip.robot));
}

TEST_CASE("After handoff B is free and a second robot still cannot enter C", "[execution-recovery]") {
    Trip trip;
    auto& g = trip.graph;
    g.node_index["D"] = 2;
    g.nodes.push_back({"D", 0, 5});
    g.edge_index["DB"] = 2;
    g.edges.push_back({"DB", 2, 0, 5});
    const auto final = trip.decide();
    REQUIRE(final.held);
    REQUIRE(final.stopped_on_node(g, &trip.robot));
    traffic::ProtectedRoute origin{"q", "other/0/s0", "map", 7, {2}, {}, 0};
    std::vector<traffic::TrafficOccupant> occupants{{"r", "map", "C", true, false},
                                                   {"q", "map", "D", true, false}};
    std::vector<traffic::ProtectedRoute> routes{origin, *final.held};
    auto next = origin;
    next.nodes.push_back(0);
    next.edges.push_back(2);
    traffic::FixedRouteGate gate;
    CHECK_FALSE(gate.check(g, occupants, routes, next).granted);
    routes.pop_back();
    CHECK(gate.check(g, occupants, routes, next).granted);
    next.nodes.push_back(1);
    next.edges.push_back(0);
    CHECK_FALSE(gate.check(g, occupants, routes, next).granted);
    ++g.version;
    CHECK_FALSE(final.stopped_on_node(g, &trip.robot));
}
