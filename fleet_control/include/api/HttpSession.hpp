#pragma once
#ifndef HTTPSESSION_HPP
#define HTTPSESSION_HPP

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/system_error.hpp>
#include <iostream>
#include <string>

#include <include/api/WebSocketBroadcaster.hpp>
#include <include/api/ApiController.hpp>


namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
namespace ip    = asio::ip;

class ApiController;

class HttpSession {
public:
    explicit HttpSession(ip::tcp::endpoint ep,
        asio::io_context& io,
        std::shared_ptr<ApiController> api,
        std::shared_ptr<WebSocketBroadcaster> wsb);
    ~HttpSession();
    void stop();
private:
    asio::awaitable<void> do_listen();
    asio::awaitable<void> do_session(beast::tcp_stream stream);
    asio::awaitable<http::message_generator> handle_request(http::request<http::string_body>&& req);
    asio::io_context& ioc;
    ip::tcp::acceptor acceptor;
    ip::tcp::endpoint endpoint;
    std::shared_ptr<ApiController> api_controller;
    std::shared_ptr<WebSocketBroadcaster> broadcaster;
};



#endif //HTTPSESSION_HPP
