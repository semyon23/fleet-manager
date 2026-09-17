
#ifndef WEBSOCKETBROADCASTER_HPP
#define WEBSOCKETBROADCASTER_HPP

#pragma once
#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <vector>
#include <mutex>
#include <string>
#include <iostream>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;

class WebSocketBroadcaster : public std::enable_shared_from_this<WebSocketBroadcaster>{
public:
    using WsStream = websocket::stream<beast::tcp_stream>;

    WebSocketBroadcaster() = default;

    void add_session(std::shared_ptr<WsStream> ws);
    void broadcast(const std::string& message);
private:
    std::mutex mutex_;
    std::vector<std::weak_ptr<WsStream>> sessions_;
};



#endif //WEBSOCKETBROADCASTER_HPP
