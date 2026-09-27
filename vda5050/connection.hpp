#pragma once

// VDA 5050 v3.0.0 — топик `connection` (6.5, 7.7).
// Публикует: робот (и брокер через last will). Подписан: fleet control.
// QoS 1, все сообщения с флагом retained.
//
// Робот при подключении ставит last will = CONNECTION_BROKEN и сразу шлёт ONLINE.
// При штатном отключении — OFFLINE, потом MQTT disconnect.
// Не использовать для проверки "здоровья" робота — это только уровень MQTT-соединения.
// headerId и timestamp у last will всегда устаревшие.

#include "common.hpp"

namespace vda5050 {

enum class ConnectionState {
    Online,            // соединение робот <-> брокер активно
    Offline,           // робот отключился штатно
    Hibernating,       // режим сна: соединение живо, state не шлётся (startHibernation)
    ConnectionBroken,  // соединение оборвалось (last will)
};
VDA5050_JSON_ENUM(ConnectionState, {
    {ConnectionState::Online, "ONLINE"},
    {ConnectionState::Offline, "OFFLINE"},
    {ConnectionState::Hibernating, "HIBERNATING"},
    {ConnectionState::ConnectionBroken, "CONNECTION_BROKEN"},
})

struct Connection {
    Header header;
    ConnectionState connection_state = ConnectionState::Offline;
};

inline void to_json(json& j, const Connection& v) {
    j = json::object();
    put_header(j, v.header);
    detail::put(j, "connectionState", v.connection_state);
}

inline void from_json(const json& j, Connection& v) {
    get_header(j, v.header);
    detail::get(j, "connectionState", v.connection_state);
}

}  // namespace vda5050
