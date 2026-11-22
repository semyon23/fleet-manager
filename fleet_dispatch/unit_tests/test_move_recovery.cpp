#include <catch2/catch_test_macros.hpp>
#include "../include/MoveRecovery.hpp"
#include "../include/OrderBuilder.hpp"
#include "../include/ActiveMission.hpp"
namespace {
using namespace move_recovery;
namespace db = fms::database::v1;
namespace v = vda5050;

struct Example {
    db::RobotSpec robot;
    db::MissionExecution head, grant;
    v::State state;

    Example() {
        robot.set_robot_id("r");
        robot.set_manufacturer("K");
        robot.set_serial_number("101");

        auto* m = head.mutable_mission();
        m->set_mission_id("m");
        m->set_robot_id("r");
        auto* node = m->add_nodes();
        node->set_id("s");
        auto* move = node->mutable_move();
        for (auto id : {"A", "B", "A", "D"}) {
            auto* n = move->add_nodes();
            n->set_node_id(id);
            n->set_map_id("map");
        }
        for (auto id : {"AB", "BA", "AD"}) move->add_edges()->set_edge_id(id);
        move->set_released_nodes(3);

        auto* a = move->mutable_nodes(3)->add_actions();
        a->set_action_type("scan");
        a->set_blocking_type(fms::v1::BLOCKING_TYPE_HARD);

        auto* s = head.mutable_status();
        s->set_mission_id("m");
        s->set_robot_id("r");
        s->set_state(fms::v1::MISSION_STATE_RUNNING);
        s->set_current_node_id("s");
        auto* step = s->add_steps();
        step->set_node_id("s");
        step->set_state(fms::v1::STEP_STATE_RUNNING);
        step->set_attempts(1);

        auto* progress = step->mutable_move();
        progress->set_last_node_id("A");
        progress->set_total_nodes(4);
        progress->set_released_nodes(3);
        progress->set_revision(1);

        auto order = build_move_update("m-s", 1, *move, 2);
        order.header.manufacturer = "K";
        order.header.serial_number = "101";

        auto* action = step->add_actions();
        action->set_action_id(order.nodes.back().actions.front().action_id);
        action->set_action_type("scan");

        auto* o = head.mutable_order();
        o->set_mission_node_id("s");
        o->set_order_id("m-s");
        o->set_order_update_id(1);
        o->set_sent_order_json(nlohmann::json(order).dump());
        head.set_checkpoint_version(3);
        grant = head;

        state.header = order.header;
        state.order_id = "m-s";
        state.order_update_id = 1;
        state.last_node_id = "B";
        state.last_node_sequence_id = 2;
        state.driving = true;
        for (int i = 2; i < 4; ++i) {
            auto& n = state.node_states.emplace_back();
            n.node_id = move->nodes(i).node_id();
            n.sequence_id = 2 * i;
            n.released = i < 3;

            auto& e = state.edge_states.emplace_back();
            e.edge_id = move->edges(i - 1).edge_id();
            e.sequence_id = 2 * i - 1;
            e.released = n.released;
        }

        state.action_states.emplace_back().action_id = action->action_id();
        auto& ack = state.instant_action_states.emplace_back();
        ack.action_id = "probe";
        ack.action_type = "stateRequest";
        ack.action_status = v::ActionStatus::Finished;
    }

    Result check() const { return reconcile(robot, head, grant, "probe", &state); }
};
}  // namespace

TEST_CASE("Move recovery reconciles one fresh state", "[move-recovery]") {
    Example x;

    SECTION("driving after Extend with absolute indices") {
        auto r = x.check();
        REQUIRE(r.decision == Decision::Attach);
        CHECK(r.last_node_index == 1u);
    }
    SECTION("second A at end of base") {
        x.state.node_states.erase(x.state.node_states.begin());
        x.state.edge_states.erase(x.state.edge_states.begin());
        x.state.last_node_id = "A";
        x.state.last_node_sequence_id = 4;
        x.state.driving = false;
        CHECK(x.check().last_node_index == 2u);
    }
    SECTION("initial order already reached destination") {
        auto* move = x.head.mutable_mission()->mutable_nodes(0)->mutable_move();
        move->set_released_nodes(4);
        auto* progress = x.head.mutable_status()->mutable_steps(0)->mutable_move();
        progress->set_released_nodes(4);
        progress->set_revision(0);

        auto order = build_move_order("m-s", *move);
        order.header = x.state.header;
        auto* o = x.head.mutable_order();
        o->set_order_update_id(0);
        o->set_sent_order_json(nlohmann::json(order).dump());
        x.grant = x.head;

        x.state.order_update_id = 0;
        x.state.last_node_id = "D";
        x.state.last_node_sequence_id = 6;
        x.state.driving = false;
        x.state.node_states.clear();
        x.state.edge_states.clear();
        x.state.action_states[0].action_status = v::ActionStatus::Finished;
        CHECK(x.check().last_node_index == 3u);
    }
    SECTION("revision without new order") {
        x.head.clear_order();
        x.head.set_checkpoint_version(4);
        x.head.mutable_status()->mutable_steps(0)->mutable_move()->set_revision(2);
        CHECK(x.check().decision == Decision::Attach);
    }
    SECTION("no state yet") {
        CHECK(reconcile(x.robot, x.head, x.grant, "probe", nullptr).decision == Decision::Wait);
    }
    SECTION("stale state even with matching order") {
        x.state.instant_action_states[0].action_id = "previous-run";
        CHECK(x.check().decision == Decision::Wait);
    }
    SECTION("previous update at the stitch stays pending through standstill") {
        x.state.order_update_id = 0;
        x.state.node_states[0].released = false;
        x.state.edge_states[0].released = false;

        x.state.driving = true;
        CHECK(x.check().decision == Decision::Wait);
        x.state.driving = false;
        CHECK(x.check().decision == Decision::Wait);
    }
    SECTION("foreign order") {
        x.state.order_id = "other";
        CHECK(x.check().decision == Decision::Conflict);
    }
    SECTION("newer update") {
        ++x.state.order_update_id;
        CHECK(x.check().decision == Decision::Conflict);
    }
    SECTION("wrong robot") {
        x.state.header.serial_number = "102";
        CHECK(x.check().decision == Decision::Conflict);
    }
    SECTION("wrong repeated node") {
        x.state.last_node_id = "A";
        CHECK(x.check().decision == Decision::Conflict);
    }
    SECTION("outside base") {
        x.state.last_node_sequence_id = 6;
        CHECK(x.check().decision == Decision::Conflict);
    }
    SECTION("missing remaining route") {
        x.state.node_states.clear();
        CHECK(x.check().decision == Decision::Conflict);
    }
    SECTION("edge released into horizon") {
        x.state.edge_states.back().released = true;
        CHECK(x.check().decision == Decision::Conflict);
    }
    SECTION("horizon action already running") {
        x.state.action_states[0].action_status = v::ActionStatus::Running;
        CHECK(x.check().decision == Decision::Conflict);
    }
    SECTION("different attempt") {
        x.head.mutable_status()->mutable_steps(0)->set_attempts(2);
        CHECK(x.check().decision == Decision::Conflict);
    }
    SECTION("invalid payload") {
        x.grant.mutable_order()->set_sent_order_json("{");
        *x.head.mutable_order() = x.grant.order();
        CHECK(x.check().decision == Decision::Conflict);
    }
}
TEST_CASE("Restore an accepted move without issuing another order", "[restore-move]") {
    using namespace std::chrono_literals;
    namespace f = fms::v1;
    using Hold = ActiveMission::RecoveryHold;
    using Clock = ActiveMission::Clock;

    Example x;
    const Clock::time_point now = Clock::time_point{} + 1h;
    google::protobuf::Timestamp wall;
    wall.set_seconds(1'000);

    auto save = [&] {
        x.grant = x.head;
    };

    auto restore = [&] {
        return ActiveMission::restore_move(
            x.robot, x.head, x.grant, "probe", x.state, now, wall);
    };

    auto arrive = [&](vda5050::ActionStatus action_status) {
        auto* move = x.head.mutable_mission()->mutable_nodes(0)->mutable_move();
        move->set_released_nodes(4);
        auto* progress = x.head.mutable_status()->mutable_steps(0)->mutable_move();
        progress->set_released_nodes(4);
        progress->set_revision(0);

        auto order = build_move_order("m-s", *move);
        order.header = x.state.header;
        auto* issued = x.head.mutable_order();
        issued->set_order_update_id(0);
        issued->set_sent_order_json(nlohmann::json(order).dump());
        save();

        x.state.order_update_id = 0;
        x.state.last_node_id = "D";
        x.state.last_node_sequence_id = 6;
        x.state.driving = false;
        x.state.node_states.clear();
        x.state.edge_states.clear();
        x.state.action_states.front().action_status = action_status;
    };

    SECTION("driving after Extend keeps full route and accepted revision") {
        auto m = restore();
        REQUIRE(m.has_value());
        REQUIRE(m->tracker() != nullptr);
        CHECK(m->tracker()->phase() == OrderTracker::Phase::Accepted);
        CHECK(m->tracker()->total_nodes() == 4);
        CHECK(m->tracker()->sent_update_id() == 1);
        CHECK(m->tracker()->confirmed_update_id() == 1);
        const auto& progress = m->status().steps(0).move();
        CHECK(progress.last_node_index() == 1);
        CHECK(progress.last_node_id() == "B");
        CHECK(progress.released_nodes() == 3);
        CHECK(progress.confirmed_revision() == 1);
        CHECK(m->recovery_hold() == Hold::None);
        CHECK_FALSE(m->order_needed());
        CHECK_FALSE(m->done());
    }

    SECTION("second A is the end of the released base") {
        x.state.node_states.erase(x.state.node_states.begin());
        x.state.edge_states.erase(x.state.edge_states.begin());
        x.state.last_node_id = "A";
        x.state.last_node_sequence_id = 4;
        x.state.driving = false;

        auto m = restore();
        REQUIRE(m.has_value());
        const auto& progress = m->status().steps(0).move();
        CHECK(progress.last_node_index() == 2);
        CHECK(progress.waiting_for_release());
        CHECK_FALSE(m->done());
        CHECK_FALSE(m->order_needed());
    }

    SECTION("arrival and standstill may be reported in separate states") {
        const auto approach = x.state;
        arrive(vda5050::ActionStatus::Finished);
        auto at_goal = x.state;
        at_goal.driving = true;

        SECTION("dispatch is attached before reaching the final node") {
            x.state = approach;
            x.state.order_update_id = at_goal.order_update_id;
            x.state.node_states.back().released = true;
            x.state.edge_states.back().released = true;
        }

        SECTION("the fresh recovery reply already names the final node") {
            x.state = at_goal;
        }

        auto m = restore();
        REQUIRE(m.has_value());
        at_goal.instant_action_states.clear();
        ++at_goal.header.header_id;
        m->on_state(at_goal);
        REQUIRE(m->recovery_hold() == Hold::None);
        CHECK(m->status().state() == f::MISSION_STATE_RUNNING);
        CHECK(m->status().steps(0).move().last_node_index() == 3);
        CHECK_FALSE(m->order_needed());

        ++at_goal.header.header_id;
        at_goal.driving = false;
        REQUIRE(m->on_state(at_goal));
        CHECK(m->status().state() == f::MISSION_STATE_COMPLETED);
        CHECK_FALSE(m->on_state(at_goal));
    }

    SECTION("arrival in the first reply finishes the last step once") {
        arrive(vda5050::ActionStatus::Finished);
        auto m = restore();
        REQUIRE(m.has_value());
        CHECK(m->status().state() == f::MISSION_STATE_COMPLETED);
        CHECK(m->status().steps(0).move().last_node_index() == 3);
        CHECK(m->status().steps(0).actions(0).status() == f::ACTION_STATUS_FINISHED);
        CHECK_FALSE(m->on_state(x.state));
        CHECK_FALSE(m->order_needed());
    }

    SECTION("finishing the move cannot start the next step during recovery") {
        arrive(vda5050::ActionStatus::Finished);
        auto* next = x.head.mutable_mission()->add_nodes();
        next->set_id("next");
        next->mutable_wait()->mutable_duration()->set_seconds(5);
        auto* pending = x.head.mutable_status()->add_steps();
        pending->set_node_id("next");
        pending->set_state(f::STEP_STATE_PENDING);
        save();

        auto m = restore();
        REQUIRE(m.has_value());
        CHECK(m->recovery_hold() == Hold::NextStep);
        CHECK(m->status().steps(1).state() == f::STEP_STATE_PENDING);
        CHECK(m->current_index() == 0);
        CHECK_FALSE(m->done());
        CHECK_FALSE(m->order_needed());
    }

    SECTION("failed action cannot trigger an automatic retry") {
        arrive(vda5050::ActionStatus::Failed);
        auto* node = x.head.mutable_mission()->mutable_nodes(0);
        node->set_on_failure(f::FAILURE_ACTION_RETRY);
        node->set_max_retries(1);
        save();

        auto m = restore();
        REQUIRE(m.has_value());
        CHECK(m->recovery_hold() == Hold::FailedOrder);
        CHECK(m->status().steps(0).attempts() == 1);
        CHECK_FALSE(m->order_needed());
        CHECK_FALSE(m->done());
    }

    SECTION("mission keeps the time spent before restart") {
        x.head.mutable_mission()->mutable_timeout()->set_seconds(10);
        x.head.mutable_status()->mutable_started_at()->set_seconds(991);
        save();

        auto m = restore();
        REQUIRE(m.has_value());
        REQUIRE(m->recovery_hold() == Hold::None);
        m->on_tick(now + 2s);
        CHECK(m->recovery_hold() == Hold::Deadline);
        CHECK(m->status().started_at().seconds() == 991);
        CHECK_FALSE(m->done());
        CHECK_FALSE(m->order_needed());
    }

    SECTION("step already expired while the robot was driving") {
        x.head.mutable_mission()->mutable_nodes(0)->mutable_timeout()->set_seconds(5);
        x.head.mutable_status()->mutable_steps(0)->mutable_started_at()->set_seconds(990);
        save();

        auto m = restore();
        REQUIRE(m.has_value());
        CHECK(m->recovery_hold() == Hold::Deadline);
        CHECK_FALSE(m->done());
        CHECK_FALSE(m->order_needed());
    }

    SECTION("missing start of a timed step prevents attachment") {
        x.head.mutable_mission()->mutable_nodes(0)->mutable_timeout()->set_seconds(5);
        save();
        CHECK_FALSE(restore().has_value());
    }

    SECTION("retry preserves attempt and requires proof of its own start if timed") {
        auto* step = x.head.mutable_status()->mutable_steps(0);
        step->set_attempts(2);
        step->mutable_started_at()->set_seconds(990);

        auto* move = x.head.mutable_mission()->mutable_nodes(0)->mutable_move();
        auto order = build_move_update("m-s-a2", 1, *move, 2);
        order.header = x.state.header;
        const auto& action_id = order.nodes.back().actions.front().action_id;
        step->mutable_actions(0)->set_action_id(action_id);
        x.state.action_states.front().action_id = action_id;
        x.state.order_id = order.order_id;
        x.head.mutable_order()->set_order_id(order.order_id);
        x.head.mutable_order()->set_sent_order_json(nlohmann::json(order).dump());
        save();

        auto m = restore();
        REQUIRE(m.has_value());
        CHECK(m->status().steps(0).attempts() == 2);
        CHECK(m->current_order_id() == "m-s-a2");
        CHECK_FALSE(m->order_needed());

        x.head.mutable_mission()->mutable_nodes(0)->mutable_timeout()->set_seconds(15);
        save();
        CHECK_FALSE(restore().has_value());
    }

    SECTION("foreign order and stale probe cannot create an ActiveMission") {
        const auto original = x.state.order_id;
        x.state.order_id = "foreign";
        CHECK_FALSE(restore().has_value());
        x.state.order_id = original;
        x.state.instant_action_states.front().action_id = "previous-probe";
        CHECK_FALSE(restore().has_value());
    }


        SECTION("ordinary telemetry advances the attached move without another probe") {
        auto m = restore();
        REQUIRE(m.has_value());

        x.state.instant_action_states.clear();
        x.state.node_states.erase(x.state.node_states.begin());
        x.state.edge_states.erase(x.state.edge_states.begin());
        x.state.last_node_id = "A";
        x.state.last_node_sequence_id = 4;
        x.state.driving = true;
        REQUIRE(m->on_state(x.state));
        REQUIRE(m->recovery_hold() == Hold::None);
        CHECK_FALSE(m->status().steps(0).move().waiting_for_release());
        CHECK_FALSE(m->done());
        CHECK_FALSE(m->order_needed());

        ++x.state.header.header_id;
        x.state.driving = false;
        CHECK(m->on_state(x.state));
        CHECK(m->recovery_hold() == Hold::None);
        CHECK(m->status().steps(0).move().last_node_index() == 2);
        CHECK(m->status().steps(0).move().waiting_for_release());
        CHECK_FALSE(m->order_needed());
        CHECK_FALSE(m->done());
    }

    SECTION("empty remaining route in the middle cannot finish the mission") {
        auto m = restore();
        REQUIRE(m.has_value());

        x.state.instant_action_states.clear();
        x.state.node_states.clear();
        x.state.edge_states.clear();
        x.state.driving = false;
        x.state.action_states.front().action_status = vda5050::ActionStatus::Finished;

        CHECK(m->on_state(x.state));
        CHECK(m->recovery_hold() == Hold::ConflictingState);
        CHECK_FALSE(m->done());
        CHECK_FALSE(m->order_needed());
        CHECK_FALSE(m->on_state(x.state));
    }

    SECTION("out of range progress is held before indexing the route") {
        auto m = restore();
        REQUIRE(m.has_value());

        x.state.last_node_sequence_id = 400;
        CHECK(m->on_state(x.state));
        CHECK(m->recovery_hold() == Hold::ConflictingState);
        CHECK(m->status().steps(0).move().last_node_index() == 1);
    }

    SECTION("deadline hold is reported once even when protobuf status stays running") {
        x.head.mutable_mission()->mutable_timeout()->set_seconds(10);
        x.head.mutable_status()->mutable_started_at()->set_seconds(991);
        save();

        auto m = restore();
        REQUIRE(m.has_value());
        CHECK(m->on_tick(now + 2s));
        CHECK(m->recovery_hold() == Hold::Deadline);
        CHECK(m->status().state() == fms::v1::MISSION_STATE_RUNNING);
        CHECK_FALSE(m->on_tick(now + 3s));
    }
}