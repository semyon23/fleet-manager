#include "Mission.hpp"

#include <array>
#include <chrono>
#include <format>
#include <stdexcept>

using nlohmann::json;

namespace {

constexpr std::array<const char*, 4> kPriorities = {"LOW", "NORMAL", "HIGH", "CRITICAL"};

// мс → "2026-11-08T10:00:00.123Z"
std::string iso(int64_t ms) {
    std::chrono::sys_time<std::chrono::milliseconds> point{std::chrono::milliseconds{ms}};
    return std::format("{:%FT%T}Z", point);
}

int64_t parse_iso(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3dZ", &year, &month, &day, &hour, &minute, &second,
                    &millis) != 7) {
        throw std::runtime_error("bad time: " + text);
                    }
    using namespace std::chrono;
    const sys_days date = year_month_day{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                                         std::chrono::day{static_cast<unsigned>(day)}};
    const auto point = date + hours{hour} + minutes{minute} + seconds{second} + milliseconds{millis};
    return point.time_since_epoch().count();
}

json time_or_null(const std::optional<int64_t>& ms) {
    return ms ? json(iso(*ms)) : json(nullptr);
}

std::optional<int64_t> time_from(const json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return parse_iso(value.get<std::string>());
}

json string_or_null(const std::string& text) {
    return text.empty() ? json(nullptr) : json(text);
}

std::string string_from(const json& value) {
    return value.is_null() ? std::string{} : value.get<std::string>();
}

}  // namespace

const char* priority_name(uint32_t priority) {
    return priority < kPriorities.size() ? kPriorities[priority] : "NORMAL";
}

std::optional<uint32_t> priority_from_name(const std::string& name) {
    for (uint32_t i = 0; i < kPriorities.size(); ++i) {
        if (name == kPriorities[i]) {
            return i;
        }
    }
    return std::nullopt;
}

bool same_request(const Mission& a, const Mission& b) {
    if (a.name != b.name || a.map_id != b.map_id || a.priority != b.priority ||
        a.requested_robot_id != b.requested_robot_id || a.steps.size() != b.steps.size()) {
        return false;
        }
    for (size_t i = 0; i < a.steps.size(); ++i) {
        if (a.steps[i].node_id != b.steps[i].node_id || a.steps[i].actions != b.steps[i].actions) {
            return false;
        }
    }
    return true;
}

json mission_json(const Mission& mission) {
    json steps = json::array();
    for (const auto& step : mission.steps) {
        steps.push_back({{"nodeId", step.node_id}, {"actions", step.actions}});
    }
    json step_states = json::array();
    for (const auto& step : mission.step_states) {
        step_states.push_back({{"state", step.state},
                               {"startedAt", time_or_null(step.started_at_ms)},
                               {"finishedAt", time_or_null(step.finished_at_ms)},
                               {"waitingForTraffic", step.waiting_for_traffic},
                               {"actions", step.actions}});
    }
    return {
        {"id", mission.id},
        {"name", mission.name},
        {"mapId", mission.map_id},
        {"priority", priority_name(mission.priority)},
        {"requestedRobotId", string_or_null(mission.requested_robot_id)},
        {"robotId", string_or_null(mission.robot_id)},
        {"steps", steps},
        {"state", mission.state},
        {"currentStep", mission.current_step ? json(*mission.current_step) : json(nullptr)},
        {"stepStates", step_states},
        {"error", mission.error ? json{{"code", mission.error->code}, {"message", mission.error->message}}
                                : json(nullptr)},
        {"createdAt", iso(mission.created_at_ms)},
        {"assignedAt", time_or_null(mission.assigned_at_ms)},
        {"startedAt", time_or_null(mission.started_at_ms)},
        {"finishedAt", time_or_null(mission.finished_at_ms)},
        {"updatedAt", iso(mission.updated_at_ms)},
        {"createdBy", nullptr},   // авторизации пока нет
    };
}

Mission mission_from_json(const json& doc) {
    Mission mission;
    mission.id = doc.at("id").get<std::string>();
    mission.name = doc.at("name").get<std::string>();
    mission.map_id = doc.at("mapId").get<std::string>();
    mission.priority = priority_from_name(doc.at("priority").get<std::string>()).value_or(1);
    mission.requested_robot_id = string_from(doc.at("requestedRobotId"));
    mission.robot_id = string_from(doc.at("robotId"));
    for (const auto& step : doc.at("steps")) {
        mission.steps.push_back({step.at("nodeId").get<std::string>(), step.at("actions")});
    }
    mission.state = doc.at("state").get<std::string>();
    if (!doc.at("currentStep").is_null()) {
        mission.current_step = doc.at("currentStep").get<int>();
    }
    for (const auto& step : doc.at("stepStates")) {
        StepState state;
        state.state = step.at("state").get<std::string>();
        state.started_at_ms = time_from(step.at("startedAt"));
        state.finished_at_ms = time_from(step.at("finishedAt"));
        state.waiting_for_traffic = step.at("waitingForTraffic").get<bool>();
        state.actions = step.at("actions");
        mission.step_states.push_back(std::move(state));
    }
    if (const json& error = doc.at("error"); !error.is_null()) {
        mission.error = MissionError{error.at("code").get<std::string>(), error.at("message").get<std::string>()};
    }
    mission.created_at_ms = parse_iso(doc.at("createdAt").get<std::string>());
    mission.assigned_at_ms = time_from(doc.at("assignedAt"));
    mission.started_at_ms = time_from(doc.at("startedAt"));
    mission.finished_at_ms = time_from(doc.at("finishedAt"));
    mission.updated_at_ms = parse_iso(doc.at("updatedAt").get<std::string>());
    return mission;
}

std::vector<MissionEvent> mission_events(const Mission* before, const Mission& after, const std::string& why) {
    // Робот события — назначенный; сняли (вернулась в очередь) — тот, от которого она ушла
    const std::string robot = after.robot_id.empty() && before ? before->robot_id : after.robot_id;

    std::vector<MissionEvent> state;
    if (!before || before->state != after.state) {
        json details = {{"state", after.state}};
        if (!why.empty()) {
            details["reason"] = why;
        }
        if (after.state == "FAILED" && after.error) {
            details["error"] = {{"code", after.error->code}, {"message", after.error->message}};
        }
        state.push_back({"STATE", robot, details});
    }

    std::vector<MissionEvent> steps;
    for (size_t i = 0; i < after.step_states.size(); ++i) {
        const StepState& now = after.step_states[i];
        const StepState* was = before && i < before->step_states.size() ? &before->step_states[i] : nullptr;
        const int index = static_cast<int>(i);

        // Действия: новый статус — событие. WAITING — ещё не начиналось, не событие
        std::vector<MissionEvent> actions;
        for (const json& action : now.actions) {
            const std::string status = action.value("status", "");
            const std::string id = action.value("actionId", "");
            if (status.empty() || status == "WAITING") {
                continue;
            }
            bool same = false;
            if (was) {
                for (const json& old : was->actions) {
                    if (old.value("actionId", "") == id && old.value("status", "") == status) {
                        same = true;
                    }
                }
            }
            if (!same) {
                actions.push_back({"ACTION", robot, {{"stepIndex", index}, {"actionId", id},
                                                     {"actionType", action.value("actionType", "")},
                                                     {"status", status}}});
            }
        }

        // Шаг: новое состояние — событие. Не начинался и отменён вместе с миссией — не событие: это видно по миссии
        const std::string old_state = was ? was->state : "PENDING";
        const bool step_changed = old_state != now.state && !(old_state == "PENDING" && now.state == "CANCELED");
        const MissionEvent step{"STEP", robot, {{"stepIndex", index}, {"state", now.state}}};
        if (step_changed && now.state == "RUNNING") {
            steps.push_back(step);   // шаг начался — до его действий
        }
        steps.insert(steps.end(), actions.begin(), actions.end());
        if (step_changed && now.state != "RUNNING") {
            steps.push_back(step);   // шаг закончился — после его действий
        }
    }

    // Миссия закончилась — её событие последним, иначе (началась, идёт) — первым
    if (after.finished()) {
        steps.insert(steps.end(), state.begin(), state.end());
        return steps;
    }
    state.insert(state.end(), steps.begin(), steps.end());
    return state;
}