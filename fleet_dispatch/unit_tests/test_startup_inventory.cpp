#include <catch2/catch_test_macros.hpp>
#include "../include/RecoveryInventory.hpp"
#include "../include/StartupInventory.hpp"

TEST_CASE("Dispatch startup inventory", "[startup-inventory]") {
    namespace db = fms::database::v1;
    db::DispatchSnapshot s;
    SECTION("confirmed empty snapshot") {
        CHECK(StartupInventory(s).reason.empty());
    }
    auto* r = s.add_robots()->mutable_spec();
    r->set_robot_id("r");
    r->set_serial_number("101");
    r->set_manufacturer("K");
    r->set_lifecycle(db::ROBOT_LIFECYCLE_ACTIVE);
    SECTION("pending delete survives startup") {
        r->set_lifecycle(db::ROBOT_LIFECYCLE_PENDING_DELETE);
        StartupInventory inventory(s);
        CHECK(inventory.deleting.contains("r"));
        CHECK(inventory.reason.empty());
    }
    SECTION("duplicate physical identity rejects the entire inventory") {
        auto copy = *r;
        copy.set_robot_id("other");
        *s.add_robots()->mutable_spec() = copy;
        CHECK_THROWS_AS(StartupInventory{s}, std::invalid_argument);
    }
    SECTION("owner recovered from an Add receipt") {
        fms::dispatch::v1::AddMissionRequest add;
        add.mutable_mission()->set_robot_id("r");
        add.mutable_mission()->set_mission_id("m");
        auto* u = s.add_unresolved();
        u->set_mission_id("m");
        u->set_add_request(add.SerializeAsString());
        StartupInventory inventory(s);
        CHECK(inventory.occupied.at("r") == "m");
        CHECK_FALSE(inventory.reason.empty());
    }
    SECTION("Move without last grant stays protected") {
        auto* e = s.add_active_missions();
        e->set_checkpoint_version(1);
        e->mutable_mission()->set_robot_id("r");
        e->mutable_mission()->set_mission_id("m");
        auto* n = e->mutable_mission()->add_nodes();
        n->set_id("move");
        n->mutable_move();
        e->mutable_status()->set_robot_id("r");
        e->mutable_status()->set_mission_id("m");
        e->mutable_status()->set_current_node_id("move");
        StartupInventory inventory(s);
        CHECK(inventory.occupied.at("r") == "m");
        CHECK(inventory.reason == "missing last grant");
    }
    SECTION("two missions cannot make the same robot free") {
        for (auto id : {"m1","m2"}) {
            auto* u = s.add_unresolved();
            u->set_mission_id(id);
            u->set_robot_id("r");
        }
        CHECK(StartupInventory(s).reason == "multiple executions on one robot");
    }
}

TEST_CASE("Fleet recovery inventory distinguishes loading from an empty fleet", "[recovery-inventory]") {
    using Reply = fms::dispatch::v1::GetRecoveryInventoryResponse;
    RecoveryInventory inventory("first"), restarted("second");
    const auto& view = inventory.snapshot();
    CHECK(view.instance_id() == "first");
    REQUIRE(view.state() == Reply::LOADING);
    const auto loading = view.revision();
    inventory.reject_startup();
    CHECK(view.state() == Reply::CONFLICT);
    inventory.update({}, {}, true);
    REQUIRE(view.state() == Reply::READY);
    CHECK(view.robots().empty());
    CHECK(view.missions().empty());
    CHECK(view.revision() > loading);
    const auto ready = view.revision();
    inventory.update({}, {}, true);
    CHECK(view.revision() == ready);
    CHECK(restarted.snapshot().instance_id() != view.instance_id());
    CHECK(restarted.snapshot().state() == Reply::LOADING);
    fms::database::v1::RecoveryRows rows;
    rows.add_robots("r");
    rows.add_deleting("r");
    inventory.update(rows, {{"r", {}}}, true);
    REQUIRE(view.blocked_robots_size() == 1);
    CHECK(view.blocked_robots(0) == "r");
}

TEST_CASE("Fleet recovery inventory versions ownership and observation changes", "[recovery-inventory]") {
    using Reply = fms::dispatch::v1::GetRecoveryInventoryResponse;
    fms::database::v1::RecoveryRows rows;
    std::map<std::string, RecoveryRobotView> live;
    for (const auto* id : {"b", "a"}) {
        rows.add_robots(id);
        live[id] = {};
        auto* mission = rows.add_missions();
        mission->set_robot_id(id);
        mission->set_mission_id(std::string("m-") + id);
    }
    rows.mutable_missions(1)->set_add_attempt(0);
    RecoveryInventory inventory("run");
    const auto& view = inventory.snapshot();
    inventory.update(rows, live, true);
    REQUIRE(view.state() == Reply::READY);
    REQUIRE(view.missions_size() == 2);
    CHECK(view.missions(0).mission_id() == "m-a");
    CHECK(view.missions(0).has_add_attempt());
    CHECK(view.missions(0).add_attempt() == 0);
    CHECK_FALSE(view.missions(1).has_add_attempt());
    REQUIRE(view.blocked_robots_size() == 2);
    CHECK(view.blocked_robots(0) == "a");
    const auto revision = view.revision();
    rows.mutable_robots()->SwapElements(0, 1);
    rows.mutable_missions()->SwapElements(0, 1);
    inventory.update(rows, live, true);
    CHECK(view.revision() == revision);

    SECTION("uncertainty changes for an existing owner") {
        rows.mutable_missions(0)->set_uncertain(true);
        inventory.update(rows, live, true);
        REQUIRE(view.state() == Reply::READY);
        CHECK(view.missions(0).uncertain());
        CHECK(view.revision() > revision);
    }
    SECTION("confirmed terminal rows disappear while a robot awaiting fresh state remains blocked") {
        rows.clear_missions();
        live["b"].blocked = true;
        inventory.update(rows, live, true);
        REQUIRE(view.state() == Reply::READY);
        CHECK(view.missions().empty());
        REQUIRE(view.blocked_robots_size() == 1);
        CHECK(view.blocked_robots(0) == "b");
        CHECK(view.revision() > revision);
        const auto waiting = view.revision();
        live["b"].blocked = false;
        inventory.update(rows, live, true);
        CHECK(view.blocked_robots().empty());
        CHECK(view.revision() > waiting);
    }
    SECTION("inconsistent evidence invalidates the previous snapshot") {
        bool consistent = true;
        SECTION("unknown owner") { rows.mutable_missions(0)->clear_robot_id(); }
        SECTION("two missions claim one owner") { rows.mutable_missions(0)->set_robot_id("b"); }
        SECTION("duplicate mission") { rows.mutable_missions(0)->set_mission_id("m-b"); }
        SECTION("robot missing from the registry") { live.erase("a"); }
        SECTION("robot missing from storage") { rows.mutable_robots()->RemoveLast(); }
        SECTION("different live mission") { live["a"].mission = "another"; }
        SECTION("durable receipt conflict") { rows.mutable_missions(0)->set_conflict(true); }
        SECTION("conflicting live order") { live["a"].conflict = true; }
        SECTION("inconsistent startup snapshot") { consistent = false; }
        inventory.update(rows, live, consistent);
        CHECK(view.state() == Reply::CONFLICT);
        CHECK(view.revision() > revision);
    }
}
