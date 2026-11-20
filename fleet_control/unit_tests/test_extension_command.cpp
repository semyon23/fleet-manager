#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "missions/ExtensionCommand.hpp"

namespace {

namespace dp = fms::dispatch::v1;
namespace fm = fms::v1;

fm::MoveProgress progress(
    std::uint32_t revision = 0,
    std::uint32_t confirmed = 0,
    std::uint32_t released = 2) {

    fm::MoveProgress result;
    result.set_revision(revision);
    result.set_confirmed_revision(confirmed);
    result.set_released_nodes(released);
    result.set_total_nodes(5);
    return result;
}

dp::ExtendMissionRequest request() {
    dp::ExtendMissionRequest r;
    r.set_mission_id("mission-1");
    r.set_move_node_id("s0");
    r.set_revision(1);
    r.set_released_nodes(4);
    return r;
}

} // namespace

TEST_CASE(
    "extension: dispatch acceptance is not robot confirmation",
    "[extension-command]") {

    ExtensionCommand c(request(), progress());

    CHECK(c.state() ==
          ExtensionCommand::State::DispatchPending);

    c.observe(progress(1, 0, 4));

    CHECK(c.state() ==
          ExtensionCommand::State::RobotPending);

    c.observe(progress(1, 1, 4));

    CHECK(c.state() ==
          ExtensionCommand::State::Confirmed);
}

TEST_CASE(
    "extension: lost reply preserves exact request",
    "[extension-command]") {

    const auto original = request();
    ExtensionCommand c(original, progress());

    // Отсутствие ответа не меняет состояние или команду.
    CHECK(c.request().SerializeAsString() ==
          original.SerializeAsString());

    c.observe(progress());

    CHECK(c.state() ==
          ExtensionCommand::State::DispatchPending);
    CHECK(c.request().SerializeAsString() ==
          original.SerializeAsString());

    c.observe(progress(1, 0, 4));

    CHECK(c.state() ==
          ExtensionCommand::State::RobotPending);
}

TEST_CASE(
    "extension: stream can confirm before RPC reply",
    "[extension-command]") {

    ExtensionCommand c(request(), progress());

    c.observe(progress(1, 1, 4));
    c.observe(progress(1, 0, 4));
    c.block("late RPC error");

    CHECK(c.state() ==
          ExtensionCommand::State::Confirmed);
}

TEST_CASE(
    "extension: old progress cannot undo dispatch acceptance",
    "[extension-command]") {

    ExtensionCommand c(request(), progress());

    c.observe(progress(1, 0, 4));
    c.observe(progress());

    CHECK(c.state() ==
          ExtensionCommand::State::RobotPending);
}

TEST_CASE(
    "extension: rejection blocks further changes",
    "[extension-command]") {

    ExtensionCommand c(request(), progress());

    auto rejected = progress(1, 0, 4);
    rejected.set_update_rejected("orderValidation");
    c.observe(rejected);

    CHECK(c.state() ==
          ExtensionCommand::State::Blocked);
    CHECK(c.reason() == "orderValidation");

    c.observe(progress(1, 1, 4));

    CHECK(c.state() ==
          ExtensionCommand::State::Blocked);
}

TEST_CASE(
    "extension: conflicting progress blocks command",
    "[extension-command]") {

    ExtensionCommand c(request(), progress());
    auto p = progress(1, 0, 4);

    SECTION("other revision") {
        p.set_revision(2);
    }
    SECTION("other base") {
        p.set_released_nodes(5);
    }
    SECTION("other route size") {
        p.set_total_nodes(6);
    }
    SECTION("impossible confirmation") {
        p.set_confirmed_revision(2);
    }

    c.observe(p);

    CHECK(c.state() ==
          ExtensionCommand::State::Blocked);
    CHECK_FALSE(c.reason().empty());
}

TEST_CASE(
    "extension: previous command must be confirmed",
    "[extension-command]") {

    auto r = request();
    auto p = progress();

    SECTION("pending previous") {
        p.set_revision(1);
        r.set_revision(2);
    }
    SECTION("previous rejection") {
        p.set_update_rejected("rejected");
    }
    SECTION("missing identity") {
        r.clear_mission_id();
    }
    SECTION("missing step") {
        r.clear_move_node_id();
    }
    SECTION("skipped revision") {
        r.set_revision(2);
    }
    SECTION("no increase") {
        r.set_released_nodes(2);
    }
    SECTION("outside route") {
        r.set_released_nodes(6);
    }
    SECTION("tail replacement") {
        r.mutable_replace_tail();
    }
    SECTION("missing released nodes") {
        r.clear_released_nodes();
    }
    SECTION("invalid previous base") {
        p.set_released_nodes(0);
    }
    SECTION("revision overflow") {
        p.set_revision(
            std::numeric_limits<std::uint32_t>::max());
        p.set_confirmed_revision(p.revision());
        r.set_revision(0);
    }

    CHECK_THROWS_AS(
        ExtensionCommand(r, p),
        std::invalid_argument);
}