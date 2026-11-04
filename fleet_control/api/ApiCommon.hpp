#ifndef FLEETMANAGERSYSTEM_APICOMMON_HPP
#define FLEETMANAGERSYSTEM_APICOMMON_HPP

#pragma once

#include <grpcpp/grpcpp.h>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "http_server.hpp"



// Ошибка gRPC от базы или dispatch → ответ с кодом контракта
HttpResponse grpc_error(const HttpRequest& req, const grpc::Status& status);

using FieldErrors = std::map<std::string, std::string>;

// 400 INVALID_INPUT, details — ошибки по полям
HttpResponse invalid_input(const HttpRequest& req, const FieldErrors& errors);

// Тело — объект JSON; иначе nullopt (ответ 400 строит вызывающий)
std::optional<nlohmann::json> parse_object(const HttpRequest& req);

// Необязательная строка: нет или null — "", не строка — ошибка в errors
std::string string_field(const nlohmann::json& body, const char* name, FieldErrors& errors);

bool valid_id(const std::string& id);

void require(const std::string& value, const char* name, FieldErrors& errors);


#endif //FLEETMANAGERSYSTEM_APICOMMON_HPP
