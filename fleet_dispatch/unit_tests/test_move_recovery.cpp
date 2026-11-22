#include <catch2/catch_test_macros.hpp>
#include "../include/MoveRecovery.hpp"
#include "../include/OrderBuilder.hpp"

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
    SECTION("previous update while stopped at stitch") {
        x.state.order_update_id = 0;
        x.state.driving = false;
        x.state.node_states[0].released = false;
        x.state.edge_states[0].released = false;
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