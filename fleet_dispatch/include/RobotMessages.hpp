#ifndef ROBOTMESSAGES_HPP
#define ROBOTMESSAGES_HPP
#pragma once

#include <boost/asio/experimental/channel.hpp>
#include <string>

namespace asio = boost::asio;

struct VdaMessage {
    std::string topic;
    std::string payload;
};

enum class MqttAction { Publish, Subscribe, Unsubscribe };

struct MqttRequest {
    MqttAction action;
    std::string topic;
    std::string payload;
};

using Outbox = asio::experimental::channel<void(boost::system::error_code, MqttRequest)>;
#endif //ROBOTMESSAGES_HPP
