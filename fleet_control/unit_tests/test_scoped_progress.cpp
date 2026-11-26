#include "ExecutionRecoveryFixture.hpp"

using namespace recovery_test;

TEST_CASE("Scoped progress follows the physical node index while recovery holds", "[scoped-progress]") {
    Trip trip(true); // B-C-B
    trip.cp.set_kind(Checkpoint::ACTIVE);
    trip.cp.mutable_status()->set_state(t::MISSION_STATE_RUNNING);
    trip.cp.mutable_status()->set_current_node_id("s0");
    auto* durable = trip.cp.mutable_status()->mutable_steps(0);
    durable->set_state(t::STEP_STATE_RUNNING);
    durable->mutable_move()->set_last_node_index(0);
    durable->mutable_move()->set_last_node_id("B");
    dp::GetMissionRequest req;
    req.set_mission_id("trip");
    req.set_expected_instance_id("new");
    req.set_expected_robot_id("r");
    req.set_expected_add_attempt(0);
    dp::GetMissionResponse reply;
    *reply.mutable_mission() = trip.cp.executed_mission();
    *reply.mutable_status() = trip.cp.status();
    reply.set_instance_id("new");
    reply.set_add_attempt(0);
    trip.robot.set_mission_id("trip");
    trip.robot.set_operating_mode(t::OPERATING_MODE_AUTOMATIC);
    trip.robot.mutable_safety()->set_emergency_stop(t::EMERGENCY_STOP_NONE);
    trip.robot.mutable_observed_order()->set_idle(false);

    auto position = [&](int index) {
        auto* p = reply.mutable_status()->mutable_steps(0)->mutable_move();
        const auto& id = reply.mission().nodes(0).move().nodes(index).node_id();
        p->set_last_node_index(index);
        p->set_last_node_id(id);
        trip.robot.set_last_node_id(id);
        trip.robot.mutable_observed_order()->set_node_sequence(2 * index);
    };

    position(0);
    const auto first = observe_move(trip.cp, req, reply, trip.robot, nullptr);
    REQUIRE(first);
    CHECK(first->parts.released.nodes.size() == 3);
    position(1);
    const auto second = observe_move(trip.cp, req, reply, trip.robot, &*first);
    REQUIRE(second);
    CHECK(second->parts.released.nodes.front() == "C");
    CHECK(second->parts.released.edges.size() == 1);
    position(2);
    const auto third = observe_move(trip.cp, req, reply, trip.robot, &*second);
    REQUIRE(third);
    CHECK(third->parts.released.nodes.size() == 1);
    CHECK(third->parts.released.nodes.front() == "B");

    SECTION("reply rolls back to the earlier B") { position(0); }
    SECTION("durable checkpoint is ahead of the first scoped reply") {
        position(0);
        durable->mutable_move()->set_last_node_index(1);
        durable->mutable_move()->set_last_node_id("C");
        CHECK_FALSE(observe_move(trip.cp, req, reply, trip.robot, nullptr));
    }
    SECTION("durable checkpoint is for a newer MoveStep attempt") { durable->set_attempts(2); }
    SECTION("previous dispatch process") { reply.set_instance_id("old"); }
    SECTION("unknown Add attempt") { reply.clear_add_attempt(); }
    SECTION("different Add attempt") { reply.set_add_attempt(1); }
    SECTION("other owner's status") { reply.mutable_status()->set_robot_id("x"); }
    SECTION("different route") { reply.mutable_mission()->mutable_nodes(0)->mutable_move()->mutable_edges(0)->set_edge_id("other"); }
    SECTION("current robot is on a different order") { trip.robot.mutable_observed_order()->set_order_id("other-s0"); }
    SECTION("checkpoint mission became terminal") { trip.cp.set_kind(Checkpoint::CONFIRMED_TERMINAL); }
    SECTION("live mission became terminal") { reply.mutable_status()->set_state(t::MISSION_STATE_COMPLETED); }
    CHECK_FALSE(observe_move(trip.cp, req, reply, trip.robot, &*third));
}
