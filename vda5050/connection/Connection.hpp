#ifndef FLEETMANAGERSYSTEM_CONNECTION_HPP
#define FLEETMANAGERSYSTEM_CONNECTION_HPP

#include <string>
#include <nlohmann/json.hpp>

enum class ConnectionState {OFFLINE, ONLINE, HIBERNATING, CONNECTION_BROKEN};

struct ConnectionMessage {
    uint32_t headerId = 0;
    std::string timestamp;
    std::string version = "3.0.0";
    std::string manufacturer;
    std::string serialNumber;
    ConnectionState connectionState{ConnectionState::OFFLINE};
};

NLOHMANN_JSON_SERIALIZE_ENUM(ConnectionState, {
    {ConnectionState::OFFLINE, "OFFLINE"},
    {ConnectionState::ONLINE, "ONLINE"},
    {ConnectionState::HIBERNATING, "HIBERNATING"},
    {ConnectionState::CONNECTION_BROKEN, "CONNECTION_BROKEN"}
})

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ConnectionMessage,
    headerId, timestamp, version, manufacturer, serialNumber, connectionState);

#endif //FLEETMANAGERSYSTEM_CONNECTION_HPP
