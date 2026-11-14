#ifndef FLEETMANAGERSYSTEM_MISSION_HPP
#define FLEETMANAGERSYSTEM_MISSION_HPP

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// Миссия fms (контракт UI, раздел 7): куда ехать и что там сделать. Её документ — объект Mission
// контракта: он же хранится в базе (MissionRecord.body_json) и отдаётся UI.
// Времена — миллисекунды Unix; в документе — ISO 8601 с миллисекундами.

struct MissionStep {
    std::string node_id;              // waypoint или станция карты
    nlohmann::json actions = nlohmann::json::array();   // [{actionType, blockingType?, actionParameters?}]
};

struct StepState {
    std::string state = "PENDING";    // PENDING | RUNNING | COMPLETED | FAILED | SKIPPED | CANCELED
    std::optional<int64_t> started_at_ms;
    std::optional<int64_t> finished_at_ms;
    bool waiting_for_traffic = false;
    nlohmann::json actions = nlohmann::json::array();   // [{actionId, actionType, status}] — от dispatch
};

struct MissionError {
    std::string code;                 // ORDER_REJECTED, ACTION_FAILED, NO_ROUTE, …
    std::string message;
};

struct Mission {
    std::string id;
    std::string name;
    std::string map_id;
    uint32_t priority = 1;            // LOW 0, NORMAL 1, HIGH 2, CRITICAL 3
    std::string requested_robot_id;   // пусто — робота выбирает fms
    std::string robot_id;             // назначенный; пусто — ещё нет
    std::vector<MissionStep> steps;

    std::string state = "QUEUED";     // QUEUED | ASSIGNED | RUNNING | PAUSED | CANCELING | COMPLETED | FAILED | CANCELED
    std::optional<int> current_step;  // индекс в steps
    std::vector<StepState> step_states;   // по одному на шаг
    std::optional<MissionError> error;

    int64_t created_at_ms = 0;
    std::optional<int64_t> assigned_at_ms;
    std::optional<int64_t> started_at_ms;
    std::optional<int64_t> finished_at_ms;
    int64_t updated_at_ms = 0;

    bool finished() const { return state == "COMPLETED" || state == "FAILED" || state == "CANCELED"; }
};

// Событие истории миссии (c3): что с ней случилось между двумя версиями документа
struct MissionEvent {
    std::string type;        // STATE | STEP | ACTION
    std::string robot_id;    // робот миссии; пусто — не назначена
    nlohmann::json details;  // STATE {state, reason?, error?}, STEP {stepIndex, state},
    // ACTION {stepIndex, actionId, actionType, status}
};

// before — прошлая версия; nullptr — миссия только что создана. why — почему сменилось состояние (может быть "").
// Порядок — как было на самом деле: началось — миссия, шаг, действия; закончилось — действия, шаг, миссия
std::vector<MissionEvent> mission_events(const Mission* before, const Mission& after, const std::string& why);

// Приоритет: число ↔ строка контракта. Неизвестная строка — nullopt
const char* priority_name(uint32_t priority);
std::optional<uint32_t> priority_from_name(const std::string& name);

// Тот же запрос оператора: name, карта, приоритет, робот, шаги. Для повтора POST с тем же id
bool same_request(const Mission& a, const Mission& b);

nlohmann::json mission_json(const Mission& mission);   // документ контракта
Mission mission_from_json(const nlohmann::json& doc);   // обратно — при старте control из базы; бросает на кривом документе

#endif //FLEETMANAGERSYSTEM_MISSION_HPP