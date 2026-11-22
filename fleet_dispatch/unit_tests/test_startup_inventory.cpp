#include <catch2/catch_test_macros.hpp>
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
