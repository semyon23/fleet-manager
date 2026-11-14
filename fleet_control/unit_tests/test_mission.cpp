// Тесты Mission (M-3b): документ миссии туда и обратно, приоритеты, «тот же запрос».

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
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

// ==================================================================== события истории (c3)

namespace {

Mission two_steps() {
    Mission mission = full_mission();
    mission.state = "QUEUED";
    mission.robot_id.clear();
    mission.error.reset();
    mission.step_states.assign(2, StepState{});
    return mission;
}

 nlohmann::json action(const std::string& id, const std::string& status) {
    return {{"actionId", id}, {"actionType", "pick"}, {"status", status}};
}

// "STATE QUEUED", "STEP 0 RUNNING", "ACTION a1 FINISHED" — так короче сравнивать порядок
std::vector<std::string> brief(const std::vector<MissionEvent>& events) {
    std::vector<std::string> lines;
    for (const auto& event : events) {
        const  nlohmann::json& d = event.details;
        if (event.type == "STATE") {
            lines.push_back("STATE " + d["state"].get<std::string>());
        } else if (event.type == "STEP") {
            lines.push_back("STEP " + std::to_string(d["stepIndex"].get<int>()) + " " + d["state"].get<std::string>());
        } else {
            lines.push_back("ACTION " + d["actionId"].get<std::string>() + " " + d["status"].get<std::string>());
        }
    }
    return lines;
}

}  // namespace

TEST_CASE("события: создание — одно QUEUED, без шагов") {
    const Mission mission = two_steps();
    const auto events = mission_events(nullptr, mission, "");
    REQUIRE(brief(events) == std::vector<std::string>{"STATE QUEUED"});
    CHECK(events[0].robot_id.empty());
    CHECK_FALSE(events[0].details.contains("reason"));
}

TEST_CASE("события: ничего не поменялось — событий нет") {
    const Mission mission = two_steps();
    CHECK(mission_events(&mission, mission, "").empty());
}

TEST_CASE("события: назначение — с причиной и роботом") {
    const Mission before = two_steps();
    Mission after = before;
    after.state = "ASSIGNED";
    after.robot_id = "amr1";
    const auto events = mission_events(&before, after, "12.3 s to the first step");
    REQUIRE(brief(events) == std::vector<std::string>{"STATE ASSIGNED"});
    CHECK(events[0].robot_id == "amr1");
    CHECK(events[0].details["reason"] == "12.3 s to the first step");
}

TEST_CASE("события: вернулась в очередь — робот тот, от которого ушла") {
    Mission before = two_steps();
    before.state = "ASSIGNED";
    before.robot_id = "amr1";
    Mission after = before;
    after.state = "QUEUED";
    after.robot_id.clear();
    const auto events = mission_events(&before, after, "robot went offline");
    REQUIRE(events.size() == 1);
    CHECK(events[0].robot_id == "amr1");
    CHECK(events[0].details["reason"] == "robot went offline");
}

TEST_CASE("события: началась — миссия, шаг, действие") {
    Mission before = two_steps();
    before.state = "ASSIGNED";
    before.robot_id = "amr1";
    Mission after = before;
    after.state = "RUNNING";
    after.step_states[0].state = "RUNNING";
    after.step_states[0].actions =  nlohmann::json::array({action("a1", "RUNNING")});
    CHECK(brief(mission_events(&before, after, "")) ==
          std::vector<std::string>{"STATE RUNNING", "STEP 0 RUNNING", "ACTION a1 RUNNING"});
}

TEST_CASE("события: WAITING и повтор статуса действия — не события") {
    Mission before = two_steps();
    before.state = "RUNNING";
    before.step_states[0].state = "RUNNING";
    before.step_states[0].actions =  nlohmann::json::array({action("a1", "RUNNING")});
    Mission after = before;
    after.step_states[0].actions =  nlohmann::json::array({action("a1", "RUNNING"), action("a2", "WAITING")});
    CHECK(mission_events(&before, after, "").empty());
}

TEST_CASE("события: закончилась — действие, шаг, миссия; не начатый шаг молча отменён") {
    Mission before = two_steps();
    before.state = "RUNNING";
    before.robot_id = "amr1";
    before.step_states[0].state = "RUNNING";
    before.step_states[0].actions =  nlohmann::json::array({action("a1", "RUNNING")});
    Mission after = before;
    after.state = "FAILED";
    after.error = MissionError{"ACTION_FAILED", "gripper jammed"};
    after.step_states[0].state = "FAILED";
    after.step_states[0].actions =  nlohmann::json::array({action("a1", "FAILED")});
    after.step_states[1].state = "CANCELED";   // PENDING → CANCELED: не событие

    const auto events = mission_events(&before, after, "");
    REQUIRE(brief(events) == std::vector<std::string>{"ACTION a1 FAILED", "STEP 0 FAILED", "STATE FAILED"});
    CHECK(events.back().details["error"] ==  nlohmann::json{{"code", "ACTION_FAILED"}, {"message", "gripper jammed"}});
}

TEST_CASE("события: следующий шаг — прошлый закончился раньше, чем начался новый") {
    Mission before = two_steps();
    before.state = "RUNNING";
    before.step_states[0].state = "RUNNING";
    Mission after = before;
    after.step_states[0].state = "COMPLETED";
    after.step_states[1].state = "RUNNING";
    CHECK(brief(mission_events(&before, after, "")) == std::vector<std::string>{"STEP 0 COMPLETED", "STEP 1 RUNNING"});
}