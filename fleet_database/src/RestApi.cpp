#include "../include/RestApi.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "api_json.hpp"
#include "RobotTable.hpp"
#include "MapTable.hpp"
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