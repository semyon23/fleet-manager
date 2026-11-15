// Тесты событий робота для журнала (M-5b): связь, ошибки VDA, готовность

#include <catch2/catch_test_macros.hpp>

#include "journal/RobotEvents.hpp"

namespace fms1 = fms::v1;
namespace dbv1 = fms::database::v1;

namespace {

fms1::RobotStatus with(fms1::ConnectionState connection) {
    fms1::RobotStatus status;
    status.set_robot_id("amr1");
    status.set_connection(connection);
    return status;
}

void add_error(fms1::RobotStatus& status, const std::string& type, fms1::ErrorLevel level) {
    auto* error = status.add_errors();
    error->set_error_type(type);
    error->set_level(level);
    error->set_description("lost");
}

dbv1::RobotAvailability availability(dbv1::AvailabilityState state) {
    dbv1::RobotAvailability a;
    a.set_state(state);
    return a;
}

}  // namespace

TEST_CASE("события робота: связь сменилась — CONNECTION; не сменилась — ничего") {
    const auto before = with(fms1::CONNECTION_STATE_ONLINE);
    const auto after = with(fms1::CONNECTION_STATE_CONNECTION_BROKEN);
    const auto events = robot_status_events(&before, after);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == "CONNECTION");
    CHECK(events[0].details["state"] == "CONNECTION_BROKEN");
    CHECK(robot_status_events(&after, after).empty());
}

TEST_CASE("события робота: первый статус — связь, если уже известна") {
    CHECK(robot_status_events(nullptr, with(fms1::CONNECTION_STATE_ONLINE)).size() == 1);
    CHECK(robot_status_events(nullptr, with(fms1::CONNECTION_STATE_UNKNOWN)).empty());
}

TEST_CASE("события робота: ошибка появилась и ушла") {
    const auto clean = with(fms1::CONNECTION_STATE_ONLINE);
    auto broken = clean;
    add_error(broken, "noRoute", fms1::ERROR_LEVEL_WARNING);

    const auto appeared = robot_status_events(&clean, broken);
    REQUIRE(appeared.size() == 1);
    CHECK(appeared[0].type == "ERROR");
    CHECK(appeared[0].details == nlohmann::json{{"errorType", "noRoute"}, {"level", "WARNING"},
                                                {"description", "lost"}, {"cleared", false}});

    const auto cleared = robot_status_events(&broken, clean);
    REQUIRE(cleared.size() == 1);
    CHECK(cleared[0].details["cleared"] == true);
}

TEST_CASE("события робота: та же ошибка с другим уровнем — ушла старая, появилась новая") {
    auto warning = with(fms1::CONNECTION_STATE_ONLINE);
    add_error(warning, "noRoute", fms1::ERROR_LEVEL_WARNING);
    auto fatal = with(fms1::CONNECTION_STATE_ONLINE);
    add_error(fatal, "noRoute", fms1::ERROR_LEVEL_FATAL);

    const auto events = robot_status_events(&warning, fatal);
    REQUIRE(events.size() == 2);
    CHECK(events[0].details["level"] == "FATAL");
    CHECK(events[0].details["cleared"] == false);
    CHECK(events[1].details["level"] == "WARNING");
    CHECK(events[1].details["cleared"] == true);
}

TEST_CASE("события робота: готовность сменилась — AVAILABILITY с причинами; первый расчёт — ничего") {
    const auto idle = availability(dbv1::AVAILABILITY_STATE_IDLE);
    auto not_ready = availability(dbv1::AVAILABILITY_STATE_NOT_READY);
    auto* reason = not_ready.add_reasons();
    reason->set_code("NOT_ON_GRAPH");
    reason->set_message("between nodes");

    const auto event = availability_event(&idle, not_ready);
    REQUIRE(event);
    CHECK(event->type == "AVAILABILITY");
    CHECK(event->details == nlohmann::json{{"state", "NOT_READY"},
                                           {"reasons", {{{"code", "NOT_ON_GRAPH"}, {"message", "between nodes"}}}}});
    CHECK_FALSE(availability_event(nullptr, idle));
    CHECK_FALSE(availability_event(&idle, idle));
}
