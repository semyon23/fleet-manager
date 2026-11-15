// Тесты правил тревог (M-5b): какие тревоги робота должны гореть при данном статусе

#include <catch2/catch_test_macros.hpp>

#include "alarms/AlarmRules.hpp"

namespace fms1 = fms::v1;

namespace {

fms1::RobotStatus online() {
    fms1::RobotStatus status;
    status.set_robot_id("amr1");
    status.set_connection(fms1::CONNECTION_STATE_ONLINE);
    status.mutable_power()->set_state_of_charge(80);
    status.mutable_safety()->set_emergency_stop(fms1::EMERGENCY_STOP_NONE);
    status.mutable_last_state_at()->set_seconds(1790000000);   // state от робота уже был
    return status;
}

void add_error(fms1::RobotStatus& status, const std::string& type, fms1::ErrorLevel level) {
    auto* error = status.add_errors();
    error->set_error_type(type);
    error->set_level(level);
    error->set_description(type + " happened");
}

std::vector<std::string> keys(const std::vector<AlarmCondition>& conditions) {
    std::vector<std::string> result;
    for (const auto& c : conditions) result.push_back(c.key);
    return result;
}

}  // namespace

TEST_CASE("тревоги: робот на связи и всё в порядке — ничего") {
    auto status = online();
    CHECK(robot_alarm_conditions("amr1", &status, 15).empty());
}

TEST_CASE("тревоги: нет связи — только ROBOT_OFFLINE, с задержкой; остальное подавлено") {
    auto status = online();
    status.set_connection(fms1::CONNECTION_STATE_CONNECTION_BROKEN);
    status.mutable_safety()->set_emergency_stop(fms1::EMERGENCY_STOP_MANUAL);
    add_error(status, "noRoute", fms1::ERROR_LEVEL_FATAL);
    status.mutable_power()->set_state_of_charge(5);

    const auto conditions = robot_alarm_conditions("amr1", &status, 15);
    REQUIRE(keys(conditions) == std::vector<std::string>{"ROBOT_OFFLINE/amr1"});
    CHECK(conditions[0].severity == "ERROR");
    CHECK(conditions[0].delay_ms == 5000);
    CHECK(conditions[0].detail == "amr1: connection CONNECTION_BROKEN");
}

TEST_CASE("тревоги: от робота ничего не было — тоже offline") {
    CHECK(keys(robot_alarm_conditions("amr1", nullptr, 15)) == std::vector<std::string>{"ROBOT_OFFLINE/amr1"});
}

TEST_CASE("тревоги: спит — ничего") {
    auto status = online();
    status.set_connection(fms1::CONNECTION_STATE_HIBERNATING);
    add_error(status, "noRoute", fms1::ERROR_LEVEL_FATAL);
    CHECK(robot_alarm_conditions("amr1", &status, 15).empty());
}

TEST_CASE("тревоги: аварийная остановка — CRITICAL") {
    auto status = online();
    status.mutable_safety()->set_emergency_stop(fms1::EMERGENCY_STOP_REMOTE);
    const auto conditions = robot_alarm_conditions("amr1", &status, 15);
    REQUIRE(conditions.size() == 1);
    CHECK(conditions[0].key == "EMERGENCY_STOP/amr1");
    CHECK(conditions[0].severity == "CRITICAL");
    CHECK(conditions[0].delay_ms == 0);
}

TEST_CASE("тревоги: ошибки VDA — одна на errorType, severity по самому серьёзному уровню") {
    auto status = online();
    add_error(status, "noRoute", fms1::ERROR_LEVEL_WARNING);
    add_error(status, "noRoute", fms1::ERROR_LEVEL_URGENT);
    add_error(status, "lidar", fms1::ERROR_LEVEL_FATAL);

    const auto conditions = robot_alarm_conditions("amr1", &status, 15);
    REQUIRE(keys(conditions) == std::vector<std::string>{"ROBOT_ERROR/amr1/lidar", "ROBOT_ERROR/amr1/noRoute"});
    CHECK(conditions[0].severity == "CRITICAL");
    CHECK(conditions[1].severity == "ERROR");
    CHECK(conditions[1].title == "Robot error: noRoute");
}

TEST_CASE("тревоги: батарея ниже порога — WARNING; заряжается — нет") {
    auto status = online();
    status.mutable_power()->set_state_of_charge(9);
    const auto conditions = robot_alarm_conditions("amr1", &status, 15);
    REQUIRE(keys(conditions) == std::vector<std::string>{"LOW_BATTERY/amr1"});
    CHECK(conditions[0].detail == "amr1: battery 9 % is below 15 %");

    status.mutable_power()->set_charging(true);
    CHECK(robot_alarm_conditions("amr1", &status, 15).empty());
    CHECK(conditions[0].delay_ms == 10000);;
}

TEST_CASE("тревоги: на связи, но state ещё не было — ничего (заряд 0 — не данные робота)") {
    auto status = online();
    status.clear_last_state_at();
    status.mutable_power()->set_state_of_charge(0);
    CHECK(robot_alarm_conditions("amr1", &status, 15).empty());
}