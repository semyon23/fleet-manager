#include "AlarmApi.hpp"

#include <chrono>
#include <format>
#include <google/protobuf/util/time_util.h>
#include <nlohmann/json.hpp>

using google::protobuf::util::TimeUtil;
using nlohmann::json;

namespace {

// Время контракта: ISO UTC с миллисекундами; нет — null
json time_json(bool present, const google::protobuf::Timestamp& time) {
    if (!present) return nullptr;
    std::chrono::sys_time<std::chrono::milliseconds> point{std::chrono::milliseconds{TimeUtil::TimestampToMilliseconds(time)}};
    return std::format("{:%FT%T}Z", point);
}

json text_or_null(const std::string& text) {
    return text.empty() ? json(nullptr) : json(text);
}

// Alarm контракта 8.1 — та же форма, что отдаёт база
json alarm_json(const fms::database::v1::AlarmRecord& alarm) {
    return {
        {"id", alarm.alarm_id()},
        {"severity", alarm.severity()},
        {"source", alarm.source()},
        {"code", alarm.code()},
        {"title", alarm.title()},
        {"detail", alarm.detail()},
        {"robotId", text_or_null(alarm.robot_id())},
        {"missionId", text_or_null(alarm.mission_id())},
        {"active", alarm.active()},
        {"createdAt", time_json(true, alarm.created_at())},
        {"clearedAt", time_json(alarm.has_cleared_at(), alarm.cleared_at())},
        {"acknowledgedAt", time_json(alarm.has_acknowledged_at(), alarm.acknowledged_at())},
        {"acknowledgedBy", text_or_null(alarm.acknowledged_by())},
    };
}

}  // namespace

AlarmApi::AlarmApi(AlarmEngine& alarms, ApiRouter& router) : alarms_(alarms) {
    router.add_route(http::verb::post, "/api/fms/alarms/{id}/ack",
        [this](const HttpRequest& req, const PathParams& params) { return ack(req, params.at("id")); });
}

asio::awaitable<HttpResponse> AlarmApi::ack(const HttpRequest& req, std::string alarm_id) {
    if (!alarms_.loaded()) {
        co_return error_response(req, http::status::service_unavailable, "UNAVAILABLE",
                                 "alarms are loading, try again later");
    }
    // Пользователей пока нет — кто подтвердил, не знаем (acknowledgedBy: null)
    auto alarm = alarms_.ack(alarm_id, "");
    if (!alarm) {
        co_return error_response(req, http::status::not_found, "NOT_FOUND",
                                 "no open alarm " + alarm_id + ": unknown or already closed");
    }
    co_return json_response(req, http::status::ok, alarm_json(*alarm).dump());
}