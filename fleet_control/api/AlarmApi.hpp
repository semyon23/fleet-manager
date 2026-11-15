#ifndef FLEETMANAGERSYSTEM_ALARMAPI_HPP
#define FLEETMANAGERSYSTEM_ALARMAPI_HPP

#pragma once
#include <boost/asio/awaitable.hpp>
#include "alarms/AlarmEngine.hpp"
#include "http_server.hpp"

// Тревоги (контракт 8.2): читает база (GET /alarms), подтверждает control — POST /alarms/{id}/ack
class AlarmApi {
public:
    AlarmApi(AlarmEngine& alarms, ApiRouter& router);

private:
    asio::awaitable<HttpResponse> ack(const HttpRequest& req, std::string alarm_id);
    AlarmEngine& alarms_;
};

#endif //FLEETMANAGERSYSTEM_ALARMAPI_HPP