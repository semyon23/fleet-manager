#include "../include/RobotApi.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "api_json.hpp"

namespace dbv1 = fms::database::v1;
namespace dpv1 = fms::dispatch::v1;
using Registry = dbv1::RobotRegistry::Stub;
using Dispatch = dpv1::FleetDispatch::Stub;
using nlohmann::json;

namespace {

HttpResponse grpc_error(const HttpRequest& req, const grpc::Status& status) {
    switch (status.error_code()) {
    case grpc::StatusCode::NOT_FOUND:
        return error_response(req, http::status::not_found, "NOT_FOUND", status.error_message());
    case grpc::StatusCode::ALREADY_EXISTS:
        return error_response(req, http::status::conflict, "ALREADY_EXISTS", status.error_message());
    case grpc::StatusCode::INVALID_ARGUMENT:
        return error_response(req, http::status::bad_request, "INVALID_INPUT", status.error_message());
    case grpc::StatusCode::FAILED_PRECONDITION:
        return error_response(req, http::status::conflict, "INVALID_STATE", status.error_message());
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
        return error_response(req, http::status::service_unavailable, "UNAVAILABLE", "service unavailable, try again later");
    default:
        spdlog::error("[robot_api] {} {}: grpc {} {}", std::string(req.method_string()), std::string(req.target()),
                      static_cast<int>(status.error_code()), status.error_message());
        return error_response(req, http::status::internal_server_error, "INTERNAL", "internal error");
    }
}

// ------------------------------------------------------------------ проверка полей

using FieldErrors = std::map<std::string, std::string>;   // поле → почему не так

HttpResponse invalid_input(const HttpRequest& req, const FieldErrors& errors) {
    json body{{"code", "INVALID_INPUT"}, {"message", "some fields are invalid"}, {"details", errors}};
    return json_response(req, http::status::bad_request, body.dump());
}

// Тело — объект JSON; иначе nullopt (ответ 400 строит вызывающий)
std::optional<json> parse_object(const HttpRequest& req) {
    auto body = json::parse(req.body(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return std::nullopt;
    }
    return body;
}

// Необязательная строка: нет или null — "", не строка — ошибка в errors
std::string string_field(const json& body, const char* name, FieldErrors& errors) {
    auto it = body.find(name);
    if (it == body.end() || it->is_null()) {
        return {};
    }
    if (!it->is_string()) {
        errors[name] = "must be a string";
        return {};
    }
    return it->get<std::string>();
}

// labels: объект со строковыми значениями; нет или null — пусто
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

// По контракту: латиница, цифры, - и _, до 64 символов
bool valid_id(const std::string& id) {
    return !id.empty() && id.size() <= 64 &&
           std::ranges::all_of(id, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

void require(const std::string& value, const char* name, FieldErrors& errors) {
    if (value.empty() && !errors.contains(name)) {
        errors[name] = "required";
    }
}

}  // namespace

RobotApi::RobotApi(Services& services, ApiRouter& router) : services_(services) {
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

    co_await add_to_dispatch(created.spec());

    dbv1::RobotRecord record;
    *record.mutable_spec() = created.spec();
    co_return json_response(req, http::status::created, robot_json(record).dump());
}

asio::awaitable<void> RobotApi::add_to_dispatch(const dbv1::RobotSpec& spec) {
    dpv1::AddRobotRequest request;
    request.set_robot_id(spec.robot_id());
    request.set_manufacturer(spec.manufacturer());
    request.set_serial_number(spec.serial_number());
    dpv1::AddRobotResponse response;
    grpc::Status status = co_await services_.call<&Dispatch::PrepareAsyncAddRobot>(*services_.dispatch, request, response);
    if (!status.ok()) {
        spdlog::warn("[robot_api] robot {} is registered but not added to dispatch: {}", spec.robot_id(),
                     status.error_message());
    }
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

    co_await finish_delete(robot_id);

    dbv1::RobotRecord record = current.robot();
    record.mutable_spec()->set_lifecycle(dbv1::ROBOT_LIFECYCLE_PENDING_DELETE);
    co_return json_response(req, http::status::accepted, robot_json(record).dump());
}

// dispatch завершает миссию (ROBOT_REMOVED) и отписывается от робота, потом запись удаляется из базы.
// Не вышло — робот остаётся PENDING_DELETE: повторный DELETE или сверка с dispatch (C-3) доведут удаление.
asio::awaitable<void> RobotApi::finish_delete(const std::string& robot_id) {
    dpv1::RemoveRobotRequest remove;
    remove.set_robot_id(robot_id);
    dpv1::RemoveRobotResponse removed;
    grpc::Status status = co_await services_.call<&Dispatch::PrepareAsyncRemoveRobot>(*services_.dispatch, remove, removed);
    if (!status.ok() && status.error_code() != grpc::StatusCode::NOT_FOUND) {   // NOT_FOUND: dispatch его и не знал
        spdlog::warn("[robot_api] robot {} stays PENDING_DELETE, dispatch did not remove it: {}", robot_id,
                     status.error_message());
        co_return;
    }

    dbv1::DeleteRobotRequest erase;
    erase.set_robot_id(robot_id);
    dbv1::DeleteRobotResponse erased;
    status = co_await services_.call<&Registry::PrepareAsyncDeleteRobot>(*services_.registry, erase, erased);
    if (!status.ok()) {
        spdlog::warn("[robot_api] robot {} stays PENDING_DELETE, database did not delete it: {}", robot_id,
                     status.error_message());
        co_return;
    }
    spdlog::info("[robot_api] robot {} deleted", robot_id);
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