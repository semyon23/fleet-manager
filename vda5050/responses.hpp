#pragma once

// VDA 5050 v3.0.0 — топик `responses` (6.9, 7.5). Опциональный.
// Публикует: fleet control. Подписан: робот. QoS 0.
//
// Ответы на запросы робота из state.zoneRequests / state.edgeRequests.
// Каждый response отвечает ровно на один requestId.
//
// Расхождение в спецификации: в 6.4.3 поле называется `responseType`,
// в таблице 7.5 — `grantType`. Раздел 7 (message specification) главнее, пишем `grantType`.
// При чтении принимаем оба варианта.

#include <optional>
#include <string>
#include <vector>

#include "common.hpp"

namespace vda5050 {

enum class GrantType { Granted, Queued, Revoked, Rejected };
VDA5050_JSON_ENUM(GrantType, {
    {GrantType::Granted, "GRANTED"},
    {GrantType::Queued, "QUEUED"},
    {GrantType::Revoked, "REVOKED"},
    {GrantType::Rejected, "REJECTED"},
})

struct Response {
    std::string request_id;
    GrantType grant_type = GrantType::Queued;
    std::optional<std::string> lease_expiry;  // ISO 8601 UTC, только вместе с GRANTED
};

inline void to_json(json& j, const Response& v) {
    j = json::object();
    detail::put(j, "requestId", v.request_id);
    detail::put(j, "grantType", v.grant_type);
    detail::put(j, "leaseExpiry", v.lease_expiry);
}

inline void from_json(const json& j, Response& v) {
    detail::get(j, "requestId", v.request_id);
    if (!j.contains("grantType") && j.contains("responseType")) {
        detail::get(j, "responseType", v.grant_type);
    } else {
        detail::get(j, "grantType", v.grant_type);
    }
    detail::get(j, "leaseExpiry", v.lease_expiry);
}

struct Responses {
    Header header;
    std::vector<Response> responses;
};

inline void to_json(json& j, const Responses& v) {
    j = json::object();
    put_header(j, v.header);
    detail::put(j, "responses", v.responses);
}

inline void from_json(const json& j, Responses& v) {
    get_header(j, v.header);
    detail::get(j, "responses", v.responses);
}

}  // namespace vda5050
