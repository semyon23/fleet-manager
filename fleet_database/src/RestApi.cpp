#include "../include/RestApi.hpp"
#include "../include/EventTable.hpp"
#include "../include/AlarmTable.hpp"
#include "../include/RobotTable.hpp"
#include "../include/MapTable.hpp"
#include "../include/MissionTable.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "api_json.hpp"
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

    constexpr int kMaxEvents = 1000;   // у миссии их десятки; больше — что-то пошло не так, страницы не нужны

    // ?from= / ?to= — время ISO (RFC 3339): 2026-11-12T14:00:00Z, с долями и смещением тоже.
    // Нет параметра — nullopt; не время — ошибка в details
    std::optional<long long> time_param(const HttpRequest& req, const char* name, nlohmann::json& details) {
        auto value = query_param(req, name);
        if (!value || value->empty()) {
            return std::nullopt;
        }
        google::protobuf::Timestamp time;
        if (!TimeUtil::FromString(*value, &time)) {
            details[name] = "must be ISO time, e.g. 2026-11-12T14:00:00Z";
            return std::nullopt;
        }
        return TimeUtil::TimestampToMilliseconds(time);
    }

    // Курсор — «<createdAt, мс>:<missionId>» последней миссии страницы. Для UI это просто строка
    std::string mission_cursor(const dbv1::MissionRecord& mission) {
        return std::to_string(TimeUtil::TimestampToMilliseconds(mission.created_at())) + ":" + mission.mission_id();
    }

        const std::set<std::string, std::less<>> kEventSources = {"MISSION", "ROBOT", "ALARM", "SYSTEM"};
    const std::set<std::string, std::less<>> kSeverities = {"INFO", "WARNING", "ERROR", "CRITICAL"};

    // ?limit= 1..200, нет — 50
    int limit_param(const HttpRequest& req, nlohmann::json& details) {
        int limit = kDefaultLimit;
        if (auto value = query_param(req, "limit")) {
            const char* end = value->data() + value->size();
            auto [ptr, error] = std::from_chars(value->data(), end, limit);
            if (error != std::errc{} || ptr != end || limit < 1 || limit > kMaxLimit) {
                details["limit"] = "must be 1.." + std::to_string(kMaxLimit);
            }
        }
        return limit;
    }

    // ?name=A,B — каждое из allowed; нет — ""
    std::string list_param(const HttpRequest& req, const char* name, const std::set<std::string, std::less<>>& allowed,
                           nlohmann::json& details) {
        std::string value = query_param(req, name).value_or("");
        for (auto part : std::views::split(std::string_view(value), ',')) {
            std::string_view item(part.begin(), part.end());
            if (!value.empty() && !allowed.contains(item)) {
                details[name] = "unknown value '" + std::string(item) + "'";
            }
        }
        return value;
    }

    // ?name=true|false; нет — nullopt
    std::optional<bool> bool_param(const HttpRequest& req, const char* name, nlohmann::json& details) {
        auto value = query_param(req, name);
        if (!value || value->empty()) return std::nullopt;
        if (*value == "true") return true;
        if (*value == "false") return false;
        details[name] = "must be true or false";
        return std::nullopt;
    }

    HttpResponse invalid_query(const HttpRequest& req, const nlohmann::json& details) {
        nlohmann::json body{{"code", "INVALID_INPUT"}, {"message", "some query parameters are invalid"},
                            {"details", details}};
        return json_response(req, http::status::bad_request, body.dump());
    }

    // {"items":[...], "nextCursor": ...} из готовых объектов JSON
    std::string page_json(const std::vector<std::string>& items, const std::optional<std::string>& next_cursor) {
        std::string body = "{\"items\":[";
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) body += ',';
            body += items[i];
        }
        body += "],\"nextCursor\":" + (next_cursor ? nlohmann::json(*next_cursor).dump() : std::string("null")) + "}";
        return body;
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

    router.add_route(http::verb::get, "/api/fms/missions/{id}/events",
            [this](const HttpRequest& req, const PathParams& params) { return list_mission_events(req, params.at("id")); });

    router.add_route(http::verb::get, "/api/fms/events",
        [this](const HttpRequest& req, const PathParams&) { return list_events(req); });
    router.add_route(http::verb::get, "/api/fms/alarms",
        [this](const HttpRequest& req, const PathParams&) { return list_alarms(req); });
    router.add_route(http::verb::get, "/api/fms/alarms/{id}",
        [this](const HttpRequest& req, const PathParams& params) { return get_alarm(req, params.at("id")); });
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

    filter.from_created_ms = time_param(req, "from", details);
    filter.to_created_ms = time_param(req, "to", details);
    if (filter.from_created_ms && filter.to_created_ms && *filter.from_created_ms >= *filter.to_created_ms) {
        details["to"] = "must be later than from";
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

// События миссии (c3) для карточки в UI → {items} по порядку. Нет миссии — 404; есть, но событий нет — пусто
asio::awaitable<HttpResponse> RestApi::list_mission_events(const HttpRequest& req, std::string mission_id) {
    bool exists = false;
    std::vector<std::string> events;
    try {
        co_await database_.run([&](pqxx::work& tx) {
            exists = select_mission(tx, mission_id).has_value();
            if (exists) {
                events = select_mission_events(tx, mission_id, kMaxEvents);
            }
        });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    if (!exists) {
        co_return error_response(req, http::status::not_found, "NOT_FOUND", "mission " + mission_id + " not found");
    }

    std::string body = "{\"items\":[";
    for (size_t i = 0; i < events.size(); ++i) {
        if (i > 0) {
            body += ',';
        }
        body += events[i];   // объект события собрала база
    }
    body += "]}";
    co_return json_response(req, http::status::ok, body);
}

// Журнал (M-5): ?source=A,B&type=A,B&robotId=&missionId=&alarmId=&from=&to=&limit=&before= → {items, nextCursor}.
// Новые сверху; курсор — id последнего события страницы
asio::awaitable<HttpResponse> RestApi::list_events(const HttpRequest& req) {
    EventFilter filter;
    nlohmann::json details = nlohmann::json::object();
    filter.sources = list_param(req, "source", kEventSources, details);
    filter.types = query_param(req, "type").value_or("");   // типов много и будут новые — не проверяем
    filter.robot_id = query_param(req, "robotId").value_or("");
    filter.mission_id = query_param(req, "missionId").value_or("");
    filter.alarm_id = query_param(req, "alarmId").value_or("");
    filter.from_ms = time_param(req, "from", details);
    filter.to_ms = time_param(req, "to", details);
    if (filter.from_ms && filter.to_ms && *filter.from_ms >= *filter.to_ms) {
        details["to"] = "must be later than from";
    }
    filter.limit = limit_param(req, details);
    if (auto before = query_param(req, "before"); before && !before->empty()) {
        long long id = 0;
        const char* end = before->data() + before->size();
        auto [ptr, error] = std::from_chars(before->data(), end, id);
        if (error == std::errc{} && ptr == end) filter.before_id = id;
        else details["before"] = "invalid cursor: pass nextCursor from the previous page";
    }
    if (!details.empty()) {
        co_return invalid_query(req, details);
    }

    std::vector<EventRow> rows;
    ++filter.limit;   // одна лишняя — узнать, есть ли следующая страница
    try {
        co_await database_.run([&](pqxx::work& tx) { rows = select_events(tx, filter); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    std::optional<std::string> next;
    if (static_cast<int>(rows.size()) == filter.limit) {
        rows.pop_back();
        next = std::to_string(rows.back().id);
    }
    std::vector<std::string> items;
    for (auto& row : rows) items.push_back(std::move(row.json));
    co_return json_response(req, http::status::ok, page_json(items, next));
}

// Тревоги (контракт 8.2): ?active=&acknowledged=&robotId=&severity=A,B&limit=&before= → {items, nextCursor}.
// Новые сверху; курсор «<createdAt, мс>:<id>», как у миссий
asio::awaitable<HttpResponse> RestApi::list_alarms(const HttpRequest& req) {
    AlarmFilter filter;
    nlohmann::json details = nlohmann::json::object();
    filter.active = bool_param(req, "active", details);
    filter.acknowledged = bool_param(req, "acknowledged", details);
    filter.robot_id = query_param(req, "robotId").value_or("");
    filter.severities = list_param(req, "severity", kSeverities, details);
    filter.limit = limit_param(req, details);
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
            filter.before_alarm_id = before->substr(colon + 1);
        } else {
            details["before"] = "invalid cursor: pass nextCursor from the previous page";
        }
    }
    if (!details.empty()) {
        co_return invalid_query(req, details);
    }

    std::vector<AlarmRow> rows;
    ++filter.limit;
    try {
        co_await database_.run([&](pqxx::work& tx) { rows = select_alarms(tx, filter); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    std::optional<std::string> next;
    if (static_cast<int>(rows.size()) == filter.limit) {
        rows.pop_back();
        next = std::to_string(rows.back().created_ms) + ":" + rows.back().alarm_id;
    }
    std::vector<std::string> items;
    for (auto& row : rows) items.push_back(std::move(row.json));
    co_return json_response(req, http::status::ok, page_json(items, next));
}

asio::awaitable<HttpResponse> RestApi::get_alarm(const HttpRequest& req, std::string alarm_id) {
    std::optional<std::string> alarm;
    try {
        co_await database_.run([&](pqxx::work& tx) { alarm = select_alarm_json(tx, alarm_id); });
    } catch (const std::exception& e) {
        co_return database_error(req, e);
    }
    if (!alarm) {
        co_return error_response(req, http::status::not_found, "NOT_FOUND", "alarm " + alarm_id + " not found");
    }
    co_return json_response(req, http::status::ok, *alarm);
}