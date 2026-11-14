#include "MissionApi.hpp"

#include <algorithm>
#include <format>
#include <nlohmann/json.hpp>
#include <random>
#include <spdlog/spdlog.h>

#include "api/ApiCommon.hpp"

namespace dbv1 = fms::database::v1;
using Registry = dbv1::RobotRegistry::Stub;
using nlohmann::json;

namespace {

constexpr const char* kBlockingTypes[] = {"NONE", "SOFT", "SINGLE", "HARD"};

// m-xxxxxxxx, если клиент не прислал свой id
std::string new_mission_id() {
    static std::mt19937 random{std::random_device{}()};
    static constexpr char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<size_t> pick(0, sizeof(kChars) - 2);
    std::string id = "m-";
    for (int i = 0; i < 8; ++i) {
        id += kChars[pick(random)];
    }
    return id;
}

// Действие шага: {actionType, blockingType?, actionParameters?}. Остальные поля (descriptor и т.п.) — как есть
void check_action(const json& action, const std::string& path, FieldErrors& errors) {
    if (!action.is_object()) {
        errors[path] = "must be an object {actionType, blockingType?, actionParameters?}";
        return;
    }
    auto type = action.find("actionType");
    if (type == action.end() || !type->is_string() || type->get_ref<const std::string&>().empty()) {
        errors[path + ".actionType"] = "required: a non-empty string";
    }
    if (auto blocking = action.find("blockingType"); blocking != action.end() && !blocking->is_null()) {
        if (!blocking->is_string() ||
            std::ranges::find(kBlockingTypes, blocking->get<std::string>()) == std::end(kBlockingTypes)) {
            errors[path + ".blockingType"] = "NONE, SOFT, SINGLE or HARD";
        }
    }
    if (auto params = action.find("actionParameters"); params != action.end() && !params->is_null()) {
        bool valid = params->is_array() && std::ranges::all_of(*params, [](const json& p) {
            return p.is_object() && p.contains("key") && p["key"].is_string() && p.contains("value");
        });
        if (!valid) {
            errors[path + ".actionParameters"] = "must be an array of {key: string, value}";
        }
    }
}

// Шаги запроса. Ошибки — с путём до поля: steps[1].nodeId, steps[0].actions[2].actionType
std::vector<MissionStep> read_steps(const json& body, FieldErrors& errors) {
    std::vector<MissionStep> steps;
    auto list = body.find("steps");
    if (list == body.end() || !list->is_array() || list->empty()) {
        errors["steps"] = "required: at least one step {nodeId, actions?}";
        return steps;
    }
    for (size_t i = 0; i < list->size(); ++i) {
        const json& item = (*list)[i];
        const std::string path = std::format("steps[{}]", i);
        if (!item.is_object()) {
            errors[path] = "must be an object {nodeId, actions?}";
            continue;
        }
        MissionStep step;
        auto node = item.find("nodeId");
        if (node == item.end() || !node->is_string() || node->get_ref<const std::string&>().empty()) {
            errors[path + ".nodeId"] = "required: a node id of the map";
        } else {
            step.node_id = node->get<std::string>();
        }
        if (auto actions = item.find("actions"); actions != item.end() && !actions->is_null()) {
            if (!actions->is_array()) {
                errors[path + ".actions"] = "must be an array";
            } else {
                for (size_t j = 0; j < actions->size(); ++j) {
                    check_action((*actions)[j], std::format("{}.actions[{}]", path, j), errors);
                }
                step.actions = *actions;
            }
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

// Повтор POST с тем же id: тот же запрос — миссия как сейчас (200), другой — 409
HttpResponse repeated(const HttpRequest& req, const Mission& request, const Mission& existing) {
    if (same_request(request, existing)) {
        return json_response(req, http::status::ok, mission_json(existing).dump());
    }
    return error_response(req, http::status::conflict, "ALREADY_EXISTS",
                          "mission " + existing.id + " already exists with different content");
}

HttpResponse not_ready(const HttpRequest& req) {
    return error_response(req, http::status::service_unavailable, "UNAVAILABLE",
                          "maps or missions are loading, try again later");
}

}  // namespace

MissionApi::MissionApi(Services& services, MapStore& maps, MissionStore& missions, MissionRunner& runner, ApiRouter& router)
        : services_(services), maps_(maps), missions_(missions), runner_(runner) {
    router.add_route(http::verb::post, "/api/fms/missions",
        [this](const HttpRequest& req, const PathParams&) { return create_mission(req); });
    router.add_route(http::verb::post, "/api/fms/missions/{id}/cancel",
        [this](const HttpRequest& req, const PathParams& params) { return cancel_mission(req, params.at("id")); });
}

// ==================================================================== POST /missions

asio::awaitable<HttpResponse> MissionApi::create_mission(const HttpRequest& req) {
    if (!maps_.loaded() || !missions_.loaded()) {
        co_return not_ready(req);
    }
    auto body = parse_object(req);
    if (!body) {
        co_return error_response(req, http::status::bad_request, "INVALID_INPUT", "body must be a JSON object");
    }

    // ---- 1. Поля запроса
    FieldErrors errors;
    Mission mission;
    std::string client_id = string_field(*body, "id", errors);
    if (!errors.contains("id") && !client_id.empty() && !valid_id(client_id)) {
        errors["id"] = "1-64 characters: letters, digits, - or _";
    }
    mission.name = string_field(*body, "name", errors);
    require(mission.name, "name", errors);
    mission.map_id = string_field(*body, "mapId", errors);
    require(mission.map_id, "mapId", errors);
    mission.requested_robot_id = string_field(*body, "requestedRobotId", errors);
    if (auto priority = body->find("priority"); priority != body->end() && !priority->is_null()) {
        auto value = priority->is_string() ? priority_from_name(priority->get<std::string>()) : std::nullopt;
        if (value) {
            mission.priority = *value;
        } else {
            errors["priority"] = "LOW, NORMAL, HIGH or CRITICAL";
        }
    }
    mission.steps = read_steps(*body, errors);
    if (!errors.empty()) {
        co_return invalid_input(req, errors);
    }

    // ---- 2. Шаги на карте: узлы есть, между соседними шагами есть путь.
    // До первого co_await: пока ждём базу, карту могут сохранить, и graph станет недействительным
    const Graph* graph = maps_.find(mission.map_id);
    if (!graph) {
        co_return invalid_input(req, {{"mapId", "map " + mission.map_id + " not found"}});
    }
    routing::Router* router = maps_.router(mission.map_id);
    std::vector<std::vector<uint32_t>> arrivals;   // куда приехать на каждом шаге
    for (size_t i = 0; i < mission.steps.size(); ++i) {
        const std::string& node_id = mission.steps[i].node_id;
        const std::string path = std::format("steps[{}].nodeId", i);
        auto node = graph->find_node(node_id);
        if (!node) {
            errors[path] = "no node " + node_id + " on map " + mission.map_id;
            continue;
        }
        arrivals.push_back(graph->arrival_nodes(*node));
        if (arrivals.back().empty()) {
            errors[path] = "station " + node_id + " has no edges and no interaction nodes: robots cannot reach it";
        }
    }
    if (!errors.empty()) {
        co_return invalid_input(req, errors);
    }
    for (size_t i = 1; i < arrivals.size(); ++i) {
        bool reachable = std::ranges::any_of(arrivals[i - 1], [&](uint32_t from) {
            return router->route(from, arrivals[i]).has_value();
        });
        if (!reachable) {
            co_return error_response(req, http::status::bad_request, "INVALID_MISSION",
                                     std::format("no route from step {} ({}) to step {} ({})", i - 1,
                                                 mission.steps[i - 1].node_id, i, mission.steps[i].node_id));
        }
    }

    // ---- 3. Робот, которого просили, есть в fms
    if (!mission.requested_robot_id.empty()) {
        dbv1::GetRobotRequest get;
        get.set_robot_id(mission.requested_robot_id);
        dbv1::GetRobotResponse robot;
        grpc::Status status = co_await services_.call<&Registry::PrepareAsyncGetRobot>(*services_.registry, get, robot);
        if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
            co_return invalid_input(req, {{"requestedRobotId", "robot " + mission.requested_robot_id + " not found"}});
        }
        if (!status.ok()) {
            co_return grpc_error(req, status);
        }
    }

    // ---- 4. id: свой от клиента или новый; такой миссии ещё не было — ни активной, ни в истории
    mission.step_states.resize(mission.steps.size());
    mission.created_at_ms = now_ms();
    mission.updated_at_ms = mission.created_at_ms;
    const bool generated = client_id.empty();
    for (int attempt = 0;; ++attempt) {
        mission.id = generated ? new_mission_id() : client_id;
        auto [status, existing] = co_await missions_.get(mission.id);
        if (!status.ok()) {
            co_return grpc_error(req, status);
        }
        if (!existing) {
            break;
        }
        if (!generated) {
            co_return repeated(req, mission, *existing);   // клиент повторил запрос
        }
        if (attempt == 4) {
            co_return error_response(req, http::status::internal_server_error, "INTERNAL", "cannot generate a mission id");
        }
    }

    // ---- 5. В базу и в очередь
    json document = mission_json(mission);
    grpc::Status status = co_await missions_.create(mission);
    if (status.error_code() == grpc::StatusCode::ALREADY_EXISTS) {
        // Такой же POST пришёл, пока мы проверяли: он уже создал миссию
        if (const Mission* other = missions_.find(mission.id)) {
            co_return repeated(req, mission, *other);
        }
    }
    if (!status.ok()) {
        co_return grpc_error(req, status);   // UNAVAILABLE → 503
    }
    spdlog::info("[mission_api] mission {} created: {} steps on map {}, priority {}", mission.id,
                 mission.steps.size(), mission.map_id, priority_name(mission.priority));
    co_return json_response(req, http::status::created, document.dump());
}

// ==================================================================== POST /missions/{id}/cancel

asio::awaitable<HttpResponse> MissionApi::cancel_mission(const HttpRequest& req, std::string mission_id) {
    if (!missions_.loaded()) {
        co_return not_ready(req);
    }

    Mission* mission = missions_.find(mission_id);
    if (!mission) {
        // Нет среди активных: уже завершена (409) или не было никогда (404)
        auto [status, existing] = co_await missions_.get(mission_id);
        if (!status.ok()) {
            co_return grpc_error(req, status);
        }
        if (!existing) {
            co_return error_response(req, http::status::not_found, "NOT_FOUND", "mission " + mission_id + " not found");
        }
        co_return error_response(req, http::status::conflict, "INVALID_STATE",
                                 "mission " + mission_id + " is already " + existing->state);
    }

    if (mission->state == "CANCELING") {
        co_return json_response(req, http::status::accepted, mission_json(*mission).dump());   // уже отменяется
    }
    if (mission->state != "QUEUED") {
        // ASSIGNED, RUNNING, PAUSED — через dispatch. После co_await указатель mission уже нельзя трогать
        grpc::Status status = co_await runner_.cancel(mission_id);
        if (!status.ok()) {
            co_return grpc_error(req, status);   // dispatch недоступен → 503
        }
        auto [get_status, current] = co_await missions_.get(mission_id);
        if (!get_status.ok()) {
            co_return grpc_error(req, get_status);
        }
        co_return json_response(req, http::status::accepted, mission_json(*current).dump());
    }


    mission->state = "CANCELED";
    mission->finished_at_ms = now_ms();
    for (auto& step : mission->step_states) {
        step.state = "CANCELED";
    }
    json document = missions_.changed(*mission, "canceled by operator");
    spdlog::info("[mission_api] mission {} cancelled while queued", mission_id);
    co_return json_response(req, http::status::accepted, document.dump());
}