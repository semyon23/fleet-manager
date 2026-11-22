#include <catch2/catch_test_macros.hpp>
#include "../include/ProbeQueue.hpp"
#include "../include/RecoveryProbe.hpp"
#include <set>

TEST_CASE("Silent robots yield probe slots to the entire fleet", "[recovery-probe]") {
    ProbeQueue q;
    ProbeQueue::Time now{};

    for (int i = 0; i < 500; ++i) q.add(std::to_string(i), now);

    std::set<std::string> seen;
    while (seen.size() < 500) {
        auto batch = q.take(now);
        REQUIRE_FALSE(batch.empty());
        REQUIRE(batch.size() <= ProbeQueue::limit);

        for (const auto& id : batch) {
            if (seen.size() < 500) REQUIRE(seen.insert(id).second);
            CHECK(q.live(id, now));
            CHECK_FALSE(q.live(id, now + ProbeQueue::timeout));
        }

        CHECK(q.take(now).empty());
        now += ProbeQueue::timeout;
    }
}

TEST_CASE("A failed send releases a slot and a removed robot stays removed",
          "[recovery-probe]") {
    ProbeQueue q;
    ProbeQueue::Time now{};

    q.add("r", now);
    q.add("r", now);
    REQUIRE(q.take(now).size() == 1);

    q.retry("r", now);
    CHECK_FALSE(q.live("r", now));
    CHECK(q.take(now).empty());
    CHECK(q.take(now + ProbeQueue::delay).size() == 1);

    q.remove("r");
    CHECK(q.empty());
}

TEST_CASE("A recovery reply belongs to this robot and this probe", "[recovery-probe]") {
    fms::database::v1::RobotSpec robot;
    robot.set_manufacturer("K");
    robot.set_serial_number("101");

    RecoveryProbe p;
    p.robot = &robot;
    p.token = "new";

    vda5050::State s;
    s.header.manufacturer = "K";
    s.header.serial_number = "101";
    auto& a = s.instant_action_states.emplace_back();
    a.action_status = vda5050::ActionStatus::Finished;
    a.action_id = "old";

    CHECK_FALSE(p.acknowledged(s));
    a.action_id = "new";
    REQUIRE(p.acknowledged(s));

    SECTION("wrong serial") {
        s.header.serial_number = "102";
        CHECK_FALSE(p.acknowledged(s));
    }

    SECTION("failed action") {
        a.action_status = vda5050::ActionStatus::Failed;
        CHECK_FALSE(p.acknowledged(s));
    }

    SECTION("duplicate id") {
        auto copy = a;
        s.instant_action_states.push_back(copy);
        CHECK_FALSE(p.acknowledged(s));
    }
}