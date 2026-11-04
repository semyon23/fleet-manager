#include "MapApi.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <random>
#include <spdlog/spdlog.h>

#include "api/ApiCommon.hpp"
#include "api_json.hpp"

namespace dbv1 = fms::database::v1;
using Control = dbv1::ControlStorage::Stub;
using Registry = dbv1::RobotRegistry::Stub;
using nlohmann::json;

namespace {

// Поля записи карты: в документ (body_json) не попадают — их хранит или ставит база.
// zoneSet собирает сервер (контракт 6.1), присланный — игнорируется.
constexpr const char* kRecordFields[] = {"id", "name", "version", "pgmDataUrl", "createdAt", "updatedAt", "zoneSet"};
constexpr const char* kLists[] = {"waypoints", "edges", "stations", "zones", "assignedRobots"};

bool is_record_field(const std::string& key) {
    return std::ranges::find(kRecordFields, key) != std::end(kRecordFields);
}

// map-xxxxxx, как генерирует фронт
std::string new_map_id() {
    static std::mt19937 random{std::random_device{}()};
    static constexpr char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<size_t> pick(0, sizeof(kChars) - 2);
    std::string id = "map-";
    for (int i = 0; i < 6; ++i) {
        id += kChars[pick(random)];
    }
    return id;
}

HttpResponse invalid_map(const HttpRequest& req, const MapCheck& check) {
    json body{{"code", "INVALID_MAP"},
              {"message", check.errors.front()},
              {"details", {{"errors", check.errors}, {"warnings", check.warnings}}}};
    return json_response(req, http::status::bad_request, body.dump());
}

HttpResponse version_conflict(const HttpRequest& req, uint32_t current_version) {
    json body{{"code", "VERSION_CONFLICT"},
              {"message", "the map was saved by someone else, reload it"},
              {"details", {{"currentVersion", current_version}}}};
    return json_response(req, http::status::conflict, body.dump());
}

HttpResponse maps_loading(const HttpRequest& req) {
    return error_response(req, http::status::service_unavailable, "UNAVAILABLE", "maps are loading, try again later");
}

}  // namespace

MapApi::MapApi(Services& services, MapStore& maps, ApiRouter& router) : services_(services), maps_(maps) {
    router.add_route(http::verb::post, "/api/fms/maps",
        [this](const HttpRequest& req, const PathParams&) { return create_map(req); });
    router.add_route(http::verb::patch, "/api/fms/maps/{id}",
        [this](const HttpRequest& req, const PathParams& params) { return update_map(req, params.at("id")); });
    router.add_route(http::verb::delete_, "/api/fms/maps/{id}",
        [this](const HttpRequest& req, const PathParams& params) { return delete_map(req, params.at("id")); });
}

// ==================================================================== POST /maps

asio::awaitable<HttpResponse> MapApi::create_map(const HttpRequest& req) {
    if (!maps_.loaded()) {
        co_return maps_loading(req);
    }
    auto body = parse_object(req);
    if (!body) {
        co_return error_response(req, http::status::bad_request, "INVALID_INPUT", "body must be a JSON object");
    }

    FieldErrors errors;
    std::string map_id = string_field(*body, "id", errors);
    std::string name = string_field(*body, "name", errors);
    std::string image = string_field(*body, "pgmDataUrl", errors);
    if (!errors.contains("id") && !map_id.empty() && !valid_id(map_id)) {
        errors["id"] = "1-64 characters: letters, digits, - or _ (the mapId robots use)";
    }
    require(name, "name", errors);
    if (!errors.empty()) {
        co_return invalid_input(req, errors);
    }
    if (map_id.empty()) {
        map_id = new_map_id();
    }

    // Документ — тело как есть, без полей записи; пустые списки — []
    json document = *body;
    for (const char* key : kRecordFields) {
        document.erase(key);
    }
    for (const char* key : kLists) {
        if (!document.contains(key) || document[key].is_null()) {
            document[key] = json::array();
        }
    }
    MapCheck check = validate_map(document);
    if (!check.errors.empty()) {
        co_return invalid_map(req, check);
    }

    dbv1::CreateMapRequest create;
    create.mutable_map()->set_map_id(map_id);
    create.mutable_map()->set_name(name);
    create.mutable_map()->set_body_json(document.dump());
    create.mutable_map()->set_image(image);
    dbv1::CreateMapResponse created;
    grpc::Status status = co_await services_.call<&Control::PrepareAsyncCreateMap>(*services_.control, create, created);
    if (!status.ok()) {
        co_return grpc_error(req, status);   // ALREADY_EXISTS → 409
    }

    Graph graph = build_graph(map_id, created.map().version(), document);
    spdlog::info("[map_api] map {} created: {} nodes, {} edges", map_id, graph.nodes.size(), graph.edges.size());
    maps_.put(std::move(graph));

    dbv1::MapRecord record = created.map();
    record.set_image(image);   // база возвращает запись без картинки
    co_return json_response(req, http::status::created, map_json(record, true).dump());
}

// ==================================================================== PATCH /maps/{id}

asio::awaitable<HttpResponse> MapApi::update_map(const HttpRequest& req, std::string map_id) {
    if (!maps_.loaded()) {
        co_return maps_loading(req);
    }
    auto body = parse_object(req);
    if (!body) {
        co_return error_response(req, http::status::bad_request, "INVALID_INPUT", "body must be a JSON object");
    }

    FieldErrors errors;
    auto version = body->find("version");
    if (version == body->end() || !version->is_number_unsigned()) {
        errors["version"] = "required: the version of the map you loaded";
    }
    if (body->contains("name") && (!(*body)["name"].is_string() || (*body)["name"].get<std::string>().empty())) {
        errors["name"] = "must be a non-empty string";
    }
    std::string image = string_field(*body, "pgmDataUrl", errors);
    if (!errors.empty()) {
        co_return invalid_input(req, errors);
    }
    const auto expected_version = version->get<uint32_t>();

    dbv1::GetMapRequest get;
    get.set_map_id(map_id);
    dbv1::GetMapResponse current;
    grpc::Status status = co_await services_.call<&Control::PrepareAsyncGetMap>(*services_.control, get, current);
    if (!status.ok()) {
        co_return grpc_error(req, status);   // NOT_FOUND → 404
    }
    if (current.map().version() != expected_version) {
        co_return version_conflict(req, current.map().version());
    }

    // Присланные поля документа заменяют прежние целиком, остальное — как было
    json document = json::parse(current.map().body_json(), nullptr, false);
    if (!document.is_object()) {
        document = json::object();
    }
    for (const auto& [key, value] : body->items()) {
        if (!is_record_field(key)) {
            document[key] = value;
        }
    }
    MapCheck check = validate_map(document);
    if (!check.errors.empty()) {
        co_return invalid_map(req, check);
    }

    dbv1::UpdateMapRequest update;
    update.mutable_map()->set_map_id(map_id);
    update.mutable_map()->set_name(body->contains("name") ? (*body)["name"].get<std::string>() : current.map().name());
    update.mutable_map()->set_body_json(document.dump());
    update.set_expected_version(expected_version);
    if (body->contains("pgmDataUrl")) {
        update.set_new_image(image);   // null → "" — картинку убрали
    }
    dbv1::UpdateMapResponse updated;
    status = co_await services_.call<&Control::PrepareAsyncUpdateMap>(*services_.control, update, updated);
    if (status.error_code() == grpc::StatusCode::ABORTED) {
        co_return co_await conflict_with_current(req, map_id);   // сохранили, пока мы ждали ответа базы
    }
    if (!status.ok()) {
        co_return grpc_error(req, status);
    }

    maps_.put(build_graph(map_id, updated.map().version(), document));
    spdlog::info("[map_api] map {} saved, version {}", map_id, updated.map().version());

    dbv1::MapRecord record = updated.map();
    record.set_image(update.has_new_image() ? update.new_image() : current.map().image());
    co_return json_response(req, http::status::ok, map_json(record, true).dump());
}


asio::awaitable<HttpResponse> MapApi::conflict_with_current(const HttpRequest& req, const std::string& map_id) {
    dbv1::GetMapRequest get;
    get.set_map_id(map_id);
    dbv1::GetMapResponse current;
    grpc::Status status = co_await services_.call<&Control::PrepareAsyncGetMap>(*services_.control, get, current);
    if (!status.ok()) {
        co_return grpc_error(req, status);
    }
    co_return version_conflict(req, current.map().version());
}

// ==================================================================== DELETE /maps/{id}

asio::awaitable<HttpResponse> MapApi::delete_map(const HttpRequest& req, std::string map_id) {
    if (!maps_.loaded()) {
        co_return maps_loading(req);
    }

    // Нельзя удалить карту, на которой стоят роботы (контракт 6.2): на какой карте робот — по его pose.mapId
    dbv1::ListRobotsRequest list;
    list.set_include_deleting(true);
    dbv1::ListRobotsResponse robots;
    grpc::Status status = co_await services_.call<&Registry::PrepareAsyncListRobots>(*services_.registry, list, robots);
    if (!status.ok()) {
        co_return grpc_error(req, status);
    }
    std::vector<std::string> on_map;
    for (const auto& robot : robots.robots()) {
        if (robot.has_status() && robot.status().has_pose() && robot.status().pose().map_id() == map_id) {
            on_map.push_back(robot.spec().robot_id());
        }
    }
    if (!on_map.empty()) {
        std::string ids;
        for (const auto& id : on_map) {
            ids += (ids.empty() ? "" : ", ") + id;
        }
        co_return error_response(req, http::status::conflict, "INVALID_STATE", "robots on the map: " + ids);
    }

    dbv1::DeleteMapRequest remove;
    remove.set_map_id(map_id);
    dbv1::DeleteMapResponse removed;
    status = co_await services_.call<&Control::PrepareAsyncDeleteMap>(*services_.control, remove, removed);
    if (!status.ok()) {
        co_return grpc_error(req, status);   // NOT_FOUND → 404
    }
    maps_.remove(map_id);
    spdlog::info("[map_api] map {} deleted", map_id);
    co_return json_response(req, http::status::no_content, "");
}
