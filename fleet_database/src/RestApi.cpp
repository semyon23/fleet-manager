#include "../include/RestApi.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "ApiJson.hpp"
#include "RobotTable.hpp"

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