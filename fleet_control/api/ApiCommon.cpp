#include "ApiCommon.hpp"
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>

using nlohmann::json;

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
        spdlog::error("[api] {} {}: grpc {} {}", std::string(req.method_string()), std::string(req.target()),
                      static_cast<int>(status.error_code()), status.error_message());
        return error_response(req, http::status::internal_server_error, "INTERNAL", "internal error");
    }
}

HttpResponse invalid_input(const HttpRequest& req, const FieldErrors& errors) {
    json body{{"code", "INVALID_INPUT"}, {"message", "some fields are invalid"}, {"details", errors}};
    return json_response(req, http::status::bad_request, body.dump());
}

std::optional<json> parse_object(const HttpRequest& req) {
    auto body = json::parse(req.body(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        return std::nullopt;
    }
    return body;
}

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

bool valid_id(const std::string& id) {
    return !id.empty() && id.size() <= 64 &&
           std::ranges::all_of(id, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

void require(const std::string& value, const char* name, FieldErrors& errors) {
    if (value.empty() && !errors.contains(name)) {
        errors[name] = "required";
    }
}