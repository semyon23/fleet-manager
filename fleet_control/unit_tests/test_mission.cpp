// Тесты Mission (M-3b): документ миссии туда и обратно, приоритеты, «тот же запрос».

#include <catch2/catch_test_macros.hpp>

#include "missions/Mission.hpp"

namespace {

Mission full_mission() {
    Mission mission;
    mission.id = "m-test0001";
    mission.name = "Pallet A3 → Dock 2";
    mission.map_id = "map_1";
    mission.priority = 2;
    mission.requested_robot_id = "amr-01";
    mission.robot_id = "amr-01";
    mission.steps = {{"st-a3", {{{"actionType", "pick"}, {"blockingType", "HARD"}}}}, {"st-dock2", nlohmann::json::array()}};
    mission.state = "FAILED";
    mission.current_step = 1;
    StepState done;
    done.state = "COMPLETED";
    done.started_at_ms = 1'790'000'001'000;
    done.finished_at_ms = 1'790'000'002'500;
    done.actions = {{{"actionId", "a-1"}, {"actionType", "pick"}, {"status", "FINISHED"}}};
    StepState failed;
    failed.state = "FAILED";
    failed.started_at_ms = 1'790'000'003'000;
    failed.waiting_for_traffic = true;
    mission.step_states = {done, failed};
    mission.error = MissionError{"NODE_UNREACHABLE", "obstacle on the line"};
    mission.created_at_ms = 1'790'000'000'000;
    mission.assigned_at_ms = 1'790'000'000'100;
    mission.started_at_ms = 1'790'000'000'200;
    mission.finished_at_ms = 1'790'000'009'999;
    mission.updated_at_ms = 1'790'000'009'999;
    return mission;
}

}  // namespace

TEST_CASE("миссия → документ → миссия: ничего не теряется") {
    Mission original = full_mission();

    Mission restored = mission_from_json(mission_json(original));

    CHECK(mission_json(restored) == mission_json(original));
    CHECK(restored.created_at_ms == original.created_at_ms);
    CHECK(restored.step_states[0].finished_at_ms == original.step_states[0].finished_at_ms);
    CHECK(restored.error->code == "NODE_UNREACHABLE");
}

TEST_CASE("документ — в формате контракта") {
    nlohmann::json doc = mission_json(full_mission());

    CHECK(doc["priority"] == "HIGH");
    CHECK(doc["createdAt"] == "2026-09-21T14:13:20.000Z");
    CHECK(doc["stepStates"][0]["finishedAt"] == "2026-09-21T14:13:22.500Z");
    CHECK(doc["stepStates"][1]["finishedAt"].is_null());
    CHECK(doc["createdBy"].is_null());
}

TEST_CASE("новая миссия: пустой робот — null, шагов-статусов нет") {
    Mission mission;
    mission.id = "m-new";
    mission.name = "Go";
    mission.map_id = "map_1";
    mission.steps = {{"n1", nlohmann::json::array()}};

    nlohmann::json doc = mission_json(mission);

    CHECK(doc["robotId"].is_null());
    CHECK(doc["requestedRobotId"].is_null());
    CHECK(doc["state"] == "QUEUED");
    CHECK(doc["priority"] == "NORMAL");
    CHECK(doc["currentStep"].is_null());
    CHECK(doc["error"].is_null());
}

TEST_CASE("приоритет: строка ↔ число") {
    CHECK(priority_from_name("LOW") == 0u);
    CHECK(priority_from_name("CRITICAL") == 3u);
    CHECK_FALSE(priority_from_name("URGENT"));
    CHECK(std::string(priority_name(2)) == "HIGH");
}

TEST_CASE("тот же запрос — только то, что прислал оператор") {
    Mission a = full_mission();
    Mission b = a;
    b.state = "QUEUED";         // состояние и времена — не часть запроса
    b.robot_id.clear();
    b.created_at_ms += 5000;
    CHECK(same_request(a, b));

    b.steps[1].node_id = "st-dock3";   // другая точка — другой запрос
    CHECK_FALSE(same_request(a, b));
}
