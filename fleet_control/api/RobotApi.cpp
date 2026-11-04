#include "RobotApi.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "api/ApiCommon.hpp"
#include "api_json.hpp"

namespace dbv1 = fms::database::v1;
namespace dpv1 = fms::dispatch::v1;
using Registry = dbv1::RobotRegistry::Stub;
using Dispatch = dpv1::FleetDispatch::Stub;
using nlohmann::json;

namespace {

void read_labels(const json& body, google::protobuf::Map<std::string, std::string>& labels, FieldErrors& errors) {
    auto it = body.find("labels");
    if (it == body.end() || it->is_null()) {
        return;
    }
    if (!it->is_object()) {
        errors["labels"] = "must be an object with string values";
        return;
    }
    for (const auto& [key, value] : it->items()) {
        if (!value.is_string()) {
            errors["labels"] = "must be an object with string values";
            return;
        }
        labels[key] = value.get<std::string>();
    }
}

}  // namespace

RobotApi::RobotApi(Services& services, FleetSync& sync, AvailabilityTracker& availability, MapStore& maps, ApiRouter& router)
    : services_(services),
      sync_(sync),
      availability_(availability),
      maps_(maps){

    router.add_route(http::verb::post, "/api/fms/robots",
        [this](const HttpRequest& req, const PathParams&) { return create_robot(req); });
    router.add_route(http::verb::patch, "/api/fms/robots/{id}",
        [this](const HttpRequest& req, const PathParams& params) { return update_robot(req, params.at("id")); });

    router.add_route(http::verb::delete_, "/api/fms/robots/{id}",
    [this](const HttpRequest& req, const PathParams& params) { return delete_robot(req, params.at("id")); });
    router.add_route(http::verb::post, "/api/fms/robots/{id}/pause",
        [this](const HttpRequest& req, const PathParams& params) { return pause_robot(req, params.at("id"), true); });
    router.add_route(http::verb::post, "/api/fms/robots/{id}/resume",
        [this](const HttpRequest& req, const PathParams& params) { return pause_robot(req, params.at("id"), false); });

    router.add_route(http::verb::post, "/api/fms/robots/{id}/position",
            [this](const HttpRequest& req, const PathParams& params) { return set_position(req, params.at("id")); });


}

// ==================================================================== POST /robots

asio::awaitable<HttpResponse> RobotApi::create_robot(const HttpRequest& req) {
    auto body = parse_object(req);
    if (!body) {
        co_return error_response(req, http::status::bad_request, "INVALID_INPUT", "body must be a JSON object");
    }

    FieldErrors errors;
    dbv1::RobotSpec spec;
    spec.set_robot_id(string_field(*body, "id", errors));
    spec.set_name(string_field(*body, "name", errors));
    spec.set_manufacturer(string_field(*body, "manufacturer", errors));
    spec.set_serial_number(string_field(*body, "serialNumber", errors));
    read_labels(*body, *spec.mutable_labels(), errors);
    if (!errors.contains("id") && !valid_id(spec.robot_id())) {
        errors["id"] = "required: 1-64 characters, letters, digits, - or _";
    }
    require(spec.manufacturer(), "manufacturer", errors);
    require(spec.serial_number(), "serialNumber", errors);
    if (!errors.empty()) {
        co_return invalid_input(req, errors);
    }

    dbv1::CreateRobotRequest create;
    *create.mutable_spec() = spec;
    dbv1::CreateRobotResponse created;
    grpc::Status status = co_await services_.call<&Registry::PrepareAsyncCreateRobot>(*services_.registry, create, created);
    if (!status.ok()) {
        co_return grpc_error(req, status);
    }
    spdlog::info("[robot_api] robot {} registered ({} / {})", spec.robot_id(), spec.manufacturer(), spec.serial_number());

    co_await sync_.add_to_dispatch(created.spec());;
    dbv1::RobotRecord record;
    *record.mutable_spec() = created.spec();
    availability_.on_robot(record);   // C-4b: availability есть сразу — по контракту она не null

    *record.mutable_spec() = created.spec();
    co_return json_response(req, http::status::created, robot_json(record).dump());
}


// ==================================================================== PATCH /robots/{id}

asio::awaitable<HttpResponse> RobotApi::update_robot(const HttpRequest& req, std::string robot_id) {
    auto body = parse_object(req);
    if (!body) {
        co_return error_response(req, http::status::bad_request, "INVALID_INPUT", "body must be a JSON object");
    }

    FieldErrors errors;
    for (const char* fixed : {"id", "manufacturer", "serialNumber"}) {
        if (body->contains(fixed)) {
            errors[fixed] = "cannot be changed: delete the robot and register it again";
        }
    }
    std::string name = string_field(*body, "name", errors);
    google::protobuf::Map<std::string, std::string> labels;
    read_labels(*body, labels, errors);
    if (!errors.empty()) {
        co_return invalid_input(req, errors);
    }

    // UpdateRobot пишет name и labels целиком: неприсланное берём из базы
    dbv1::GetRobotRequest get;
    get.set_robot_id(robot_id);
    dbv1::GetRobotResponse current;
    grpc::Status status = co_await services_.call<&Registry::PrepareAsyncGetRobot>(*services_.registry, get, current);
    if (!status.ok()) {
        co_return grpc_error(req, status);
    }

    dbv1::UpdateRobotRequest update;
    *update.mutable_spec() = current.robot().spec();
    if (body->contains("name")) {
        update.mutable_spec()->set_name(name);   // null → "" → в ответе name = id
    }
    if (body->contains("labels")) {
        *update.mutable_spec()->mutable_labels() = labels;
    }
    dbv1::UpdateRobotResponse updated;
    status = co_await services_.call<&Registry::PrepareAsyncUpdateRobot>(*services_.registry, update, updated);
    if (!status.ok()) {
        co_return grpc_error(req, status);
    }

    dbv1::RobotRecord record = current.robot();   // статус — тот, что был; spec — новый
    *record.mutable_spec() = updated.spec();
    co_return json_response(req, http::status::ok, robot_json(record).dump());
}

asio::awaitable<HttpResponse> RobotApi::delete_robot(const HttpRequest& req, std::string robot_id) {
    dbv1::GetRobotRequest get;
    get.set_robot_id(robot_id);
    dbv1::GetRobotResponse current;
    grpc::Status status = co_await services_.call<&Registry::PrepareAsyncGetRobot>(*services_.registry, get, current);
    if (!status.ok()) {
        co_return grpc_error(req, status);
    }

    // Занят ли робот — пока по статусу от dispatch (миссий control ещё нет)
    const std::string& mission_id = current.robot().status().mission_id();
    bool force = query_param(req, "force") == "true";
    if (!mission_id.empty() && !force) {
        co_return error_response(req, http::status::conflict, "ROBOT_BUSY",
                                 "robot " + robot_id + " is executing mission " + mission_id +
                                 "; use ?force=true to cancel it");
    }

    // С этого шага удаление принято: дальше отвечаем 202, даже если dispatch не ответит
    dbv1::SetRobotDeletingRequest mark;
    mark.set_robot_id(robot_id);
    dbv1::SetRobotDeletingResponse marked;
    status = co_await services_.call<&Registry::PrepareAsyncSetRobotDeleting>(*services_.registry, mark, marked);
    if (!status.ok()) {
        co_return grpc_error(req, status);
    }
    spdlog::info("[robot_api] robot {} is being deleted{}", robot_id,
                 mission_id.empty() ? "" : ", its mission " + mission_id + " is canceled");

    availability_.set_deleting(robot_id);   // C-4b
    co_await sync_.finish_delete(robot_id);

    dbv1::RobotRecord record = current.robot();
    record.mutable_spec()->set_lifecycle(dbv1::ROBOT_LIFECYCLE_PENDING_DELETE);
    co_return json_response(req, http::status::accepted, robot_json(record).dump());
}


// ==================================================================== pause / resume

// VDA startPause / stopPause. 202 — команда отправлена; результат — status.paused при следующем опросе
asio::awaitable<HttpResponse> RobotApi::pause_robot(const HttpRequest& req, std::string robot_id, bool pause) {
    grpc::Status status;
    if (pause) {
        dpv1::PauseRobotRequest request;
        request.set_robot_id(robot_id);
        dpv1::PauseRobotResponse response;
        status = co_await services_.call<&Dispatch::PrepareAsyncPauseRobot>(*services_.dispatch, request, response);
    } else {
        dpv1::ResumeRobotRequest request;
        request.set_robot_id(robot_id);
        dpv1::ResumeRobotResponse response;
        status = co_await services_.call<&Dispatch::PrepareAsyncResumeRobot>(*services_.dispatch, request, response);
    }
    if (!status.ok()) {
        co_return grpc_error(req, status);   // NOT_FOUND — dispatch не знает робота
    }
    co_return json_response(req, http::status::accepted, "{}");
}

asio::awaitable<HttpResponse> RobotApi::set_position(const HttpRequest& req, std::string robot_id) {
    if (!maps_.loaded()) {
        co_return error_response(req, http::status::service_unavailable, "UNAVAILABLE", "maps are loading, try again later");
    }
    auto body = parse_object(req);
    if (!body) {
        co_return error_response(req, http::status::bad_request, "INVALID_INPUT", "body must be a JSON object");
    }

    FieldErrors errors;
    std::string map_id = string_field(*body, "mapId", errors);
    std::string node_id = string_field(*body, "nodeId", errors);
    require(map_id, "mapId", errors);
    require(node_id, "nodeId", errors);
    std::optional<double> theta;
    if (auto it = body->find("theta"); it != body->end() && !it->is_null()) {
        if (it->is_number()) {
            theta = it->get<double>();
        } else {
            errors["theta"] = "must be a number, radians";
        }
    }
    if (!errors.empty()) {
        co_return invalid_input(req, errors);
    }

    const Graph* graph = maps_.find(map_id);
    if (!graph) {
        co_return error_response(req, http::status::not_found, "NOT_FOUND", "map " + map_id + " not found");
    }
    auto found = graph->nodes.find(node_id);
    if (found == graph->nodes.end()) {
        co_return invalid_input(req, {{"nodeId", "no node " + node_id + " on map " + map_id}});
    }
    const GraphNode node = found->second;   // копия: пока ждём gRPC, карту могут сохранить и граф заменится

    dbv1::GetRobotRequest get;
    get.set_robot_id(robot_id);
    dbv1::GetRobotResponse current;
    grpc::Status status = co_await services_.call<&Registry::PrepareAsyncGetRobot>(*services_.registry, get, current);
    if (!status.ok()) {
        co_return grpc_error(req, status);   // NOT_FOUND → 404
    }
    const auto& robot = current.robot();
    if (robot.has_status() && !robot.status().mission_id().empty()) {
        // По VDA робот не примет initializePosition, пока у него есть order
        co_return error_response(req, http::status::conflict, "ROBOT_BUSY",
                                 "robot " + robot_id + " is executing mission " + robot.status().mission_id() +
                                 "; cancel it first");
    }
    // Настоящей ориентации робота fms не знает: из запроса, иначе узла, иначе прежняя
    if (!theta) {
        theta = node.theta;
    }
    if (!theta && robot.has_status() && robot.status().has_pose()) {
        theta = robot.status().pose().theta();
    }

    dpv1::InitializePositionRequest request;
    request.set_robot_id(robot_id);
    request.mutable_pose()->set_x(node.x);
    request.mutable_pose()->set_y(node.y);
    request.mutable_pose()->set_theta(theta.value_or(0.0));
    request.mutable_pose()->set_map_id(map_id);
    request.set_last_node_id(node_id);
    dpv1::InitializePositionResponse response;
    status = co_await services_.call<&Dispatch::PrepareAsyncInitializePosition>(*services_.dispatch, request, response);
    if (!status.ok()) {
        co_return grpc_error(req, status);   // FAILED_PRECONDITION (робот не ONLINE) → 409 INVALID_STATE
    }
    spdlog::info("[robot_api] robot {} placed on node {} of map {} ({:.2f}, {:.2f})", robot_id, node_id, map_id,
                 node.x, node.y);
    co_return json_response(req, http::status::accepted, "{}");
}