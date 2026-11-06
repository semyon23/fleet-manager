#include "../include/RestApi.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "api_json.hpp"
#include "RobotTable.hpp"
#include "MapTable.hpp"

#include "MissionTable.hpp"
#include <charconv>
#include <google/protobuf/util/time_util.h>
#include <ranges>
#include <set>

using google::protobuf::util::TimeUtil;
namespace {

HttpResponse database_error(const HttpRequest& req, const std::exception& e) {
    if (dynamic_cast<const pqxx::broken_connection*>(&e)) {
        spdlog::error("[rest_api] database connection lost: {}", e.what());
        return error_response(req, http::status::service_unavailable, "UNAVAILABLE",
                              "database unavailable, try again later");
    }
    spdlog::error("[rest_api] {} {}: {}", std::string(req.method_string()), std::string(req.target()), e.what());
    return error_response(req, http::status::internal_server_error, "INTERNAL", "database error");
}


    const std::set<std::string, std::less<>> kMissionStates = {
    "QUEUED", "ASSIGNED", "RUNNING", "PAUSED", "CANCELING", "COMPLETED", "FAILED", "CANCELED"};
    constexpr int kDefaultLimit = 50;
    constexpr int kMaxLimit = 200;

    // Курсор — «<createdAt, мс>:<missionId>» последней миссии страницы. Для UI это просто строка
    std::string mission_cursor(const dbv1::MissionRecord& mission) {
        return std::to_string(TimeUtil::TimestampToMilliseconds(mission.created_at())) + ":" + mission.mission_id();
    }
}  // namespace

RestApi::RestApi(Database& database, ApiRouter& router) : database_(database) {
    router.add_route(http::verb::get, "/api/fms/robots",
        [this](const HttpRequest& req, const PathParams&) { return list_robots(req); });
    router.add_route(http::verb::get, "/api/fms/robots/{id}",
        [this](const HttpRequest& req, const PathParams& params) { return get_robot(req, params.at("id")); });
    router.add_route(http::verb::get, "/api/fms/robots/{id}/factsheet",
        [this](const HttpRequest& req, const PathParams& params) { return get_factsheet(req, params.at("id")); });

    router.add_route(http::verb::get, "/api/fms/maps",
            [this](const HttpRequest& req, const PathParams&) { return list_maps(req); });
    router.add_route(http::verb::get, "/api/fms/maps/{id}",
        [this](const HttpRequest& req, const PathParams& params) { return get_map(req, params.at("id")); });

    router.add_route(http::verb::get, "/api/fms/missions",
            [this](const HttpRequest& req, const PathParams&) { return list_missions(req); });
    router.add_route(http::verb::get, "/api/fms/missions/{id}",
        [this](const HttpRequest& req, const PathParams& params) { return get_mission(req, params.at("id")); });
}

asio::awaitable<HttpResponse> RestApi::list_robots(const HttpRequest& req) {
    std::vector<dbv1::RobotRecord> robots;
    try {
        co_await database_.run([&](pqxx::work& tx) { robots = select_robots(tx, true); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    nlohmann::json body = nlohmann::json::array();
    for (const auto& robot : robots) {
        body.push_back(robot_json(robot));
    }
    co_return json_response(req, http::status::ok, body.dump());
}

asio::awaitable<HttpResponse> RestApi::get_robot(const HttpRequest& req, std::string robot_id) {
    std::optional<dbv1::RobotRecord> robot;
    try {
        co_await database_.run([&](pqxx::work& tx) { robot = select_robot(tx, robot_id); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    if (!robot) {
        co_return error_response(req, http::status::not_found, "NOT_FOUND", "robot " + robot_id + " not found");
    }
    co_return json_response(req, http::status::ok, robot_json(*robot).dump());
}

asio::awaitable<HttpResponse> RestApi::get_factsheet(const HttpRequest& req, std::string robot_id) {
    std::optional<dbv1::GetFactsheetResponse> factsheet;
    try {
        co_await database_.run([&](pqxx::work& tx) { factsheet = select_factsheet(tx, robot_id); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    if (!factsheet) {
        co_return error_response(req, http::status::not_found, "NOT_FOUND", "no factsheet for robot " + robot_id);
    }
    co_return json_response(req, http::status::ok, factsheet->factsheet_json());   // как прислал робот
}

// M-1. Список — только метаданные (контракт, раздел 6.2): граф и картинку UI берёт из GET /maps/{id}
asio::awaitable<HttpResponse> RestApi::list_maps(const HttpRequest& req) {
    std::vector<dbv1::MapRecord> maps;
    try {
        co_await database_.run([&](pqxx::work& tx) { maps = select_maps(tx); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    nlohmann::json body = nlohmann::json::array();
    for (const auto& map : maps) {
        body.push_back(map_json(map, false));
    }
    co_return json_response(req, http::status::ok, body.dump());
}

asio::awaitable<HttpResponse> RestApi::get_map(const HttpRequest& req, std::string map_id) {
    std::optional<dbv1::MapRecord> map;
    try {
        co_await database_.run([&](pqxx::work& tx) { map = select_map(tx, map_id); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    if (!map) {
        co_return error_response(req, http::status::not_found, "NOT_FOUND", "map " + map_id + " not found");
    }
    co_return json_response(req, http::status::ok, map_json(*map, true).dump());
}

// ==================================================================== миссии (M-3)

// Контракт 7.2: ?state=A,B&robotId=&mapId=&limit=&before= → {items, nextCursor}.
// body миссии в базе — объект Mission контракта целиком, его и отдаём, не разбирая
asio::awaitable<HttpResponse> RestApi::list_missions(const HttpRequest& req) {
    MissionFilter filter;
    nlohmann::json details = nlohmann::json::object();

    filter.states = query_param(req, "state").value_or("");
    if (!filter.states.empty()) {
        for (auto part : std::views::split(std::string_view(filter.states), ',')) {
            std::string_view state(part.begin(), part.end());
            if (!kMissionStates.contains(state)) {
                details["state"] = "unknown state '" + std::string(state) + "'";
            }
        }
    }
    filter.robot_id = query_param(req, "robotId").value_or("");
    filter.map_id = query_param(req, "mapId").value_or("");

    filter.limit = kDefaultLimit;
    if (auto limit = query_param(req, "limit")) {
        const char* end = limit->data() + limit->size();
        auto [ptr, error] = std::from_chars(limit->data(), end, filter.limit);
        if (error != std::errc{} || ptr != end || filter.limit < 1 || filter.limit > kMaxLimit) {
            details["limit"] = "must be 1.." + std::to_string(kMaxLimit);
        }
    }
    if (auto before = query_param(req, "before"); before && !before->empty()) {
        auto colon = before->find(':');
        long long created_ms = 0;
        bool valid = colon != std::string::npos;
        if (valid) {
            auto [ptr, error] = std::from_chars(before->data(), before->data() + colon, created_ms);
            valid = error == std::errc{} && ptr == before->data() + colon;
        }
        if (valid) {
            filter.before_created_ms = created_ms;
            filter.before_mission_id = before->substr(colon + 1);
        } else {
            details["before"] = "invalid cursor: pass nextCursor from the previous page";
        }
    }
    if (!details.empty()) {
        nlohmann::json body{{"code", "INVALID_INPUT"}, {"message", "some query parameters are invalid"},
                            {"details", details}};
        co_return json_response(req, http::status::bad_request, body.dump());
    }

    std::vector<dbv1::MissionRecord> missions;
    ++filter.limit;   // одна лишняя — узнать, есть ли следующая страница
    try {
        co_await database_.run([&](pqxx::work& tx) { missions = select_missions(tx, filter); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    std::string next_cursor = "null";
    if (static_cast<int>(missions.size()) == filter.limit) {
        missions.pop_back();
        next_cursor = nlohmann::json(mission_cursor(missions.back())).dump();
    }

    std::string body = "{\"items\":[";
    for (size_t i = 0; i < missions.size(); ++i) {
        if (i > 0) {
            body += ',';
        }
        body += missions[i].body_json();   // JSONB из базы — готовый JSON
    }
    body += "],\"nextCursor\":" + next_cursor + "}";
    co_return json_response(req, http::status::ok, body);
}

asio::awaitable<HttpResponse> RestApi::get_mission(const HttpRequest& req, std::string mission_id) {
    std::optional<dbv1::MissionRecord> mission;
    try {
        co_await database_.run([&](pqxx::work& tx) { mission = select_mission(tx, mission_id); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    if (!mission) {
        co_return error_response(req, http::status::not_found, "NOT_FOUND", "mission " + mission_id + " not found");
    }
    co_return json_response(req, http::status::ok, mission->body_json());
}