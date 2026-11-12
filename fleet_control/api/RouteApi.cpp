#include "RouteApi.hpp"
#include <nlohmann/json.hpp>

RouteApi::RouteApi(MissionRunner& runner, ApiRouter& router) : runner_(runner) {
    router.add_route(http::verb::get, "/api/fms/routes",
        [this](const HttpRequest& req, const PathParams&) { return list(req); });
}

asio::awaitable<HttpResponse> RouteApi::list(const HttpRequest& req) {
    const std::string map_id = query_param(req, "mapId").value_or("");

    nlohmann::json items = nlohmann::json::array();
    for (const auto& route : runner_.routes(map_id)) {
        items.push_back({
            {"robotId", route.robot_id},
            {"mapId", route.map_id},
            {"missionId", route.mission_id},
            {"stepIndex", route.step_index},
            {"released", {{"nodes", route.nodes}, {"edges", route.edges}}},
            // Пока трафика нет, весь маршрут шага разрешён сразу — неразрешённой части нет (T1)
            {"planned", {{"nodes", nlohmann::json::array()}, {"edges", nlohmann::json::array()}}},
            {"goals", route.goals},
            {"nextMissionId", route.next_mission_id.empty() ? nlohmann::json(nullptr)
                                                            : nlohmann::json(route.next_mission_id)},
        });
    }
    co_return json_response(req, http::status::ok, nlohmann::json{{"items", items}}.dump());
}