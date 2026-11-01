#ifndef FLEETMANAGERSYSTEM_ENV_CONFIG_HPP
#define FLEETMANAGERSYSTEM_ENV_CONFIG_HPP

#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <string>

inline std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}


inline boost::asio::ip::tcp::endpoint endpoint_from(const std::string& address) {
    auto colon = address.rfind(':');
    return {boost::asio::ip::make_address(address.substr(0, colon)),
            static_cast<unsigned short>(std::stoi(address.substr(colon + 1)))};
}

#endif //FLEETMANAGERSYSTEM_ENV_CONFIG_HPP
