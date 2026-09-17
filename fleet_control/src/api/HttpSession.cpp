
#include "include/api/HttpSession.hpp"
#include <print>

HttpSession::HttpSession(ip::tcp::endpoint ep, asio::io_context& io, std::shared_ptr<ApiController> api, std::shared_ptr<WebSocketBroadcaster> wsb)
    : endpoint(ep), ioc(io), acceptor(io), api_controller(api), broadcaster(wsb){

    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
    asio::co_spawn(ioc, do_listen(), asio::detached);
}

HttpSession::~HttpSession(){
    // boost::system::error_code ec;
    // acceptor.close(ec);
}

void HttpSession::stop(){
    boost::system::error_code ec;
    if (acceptor.is_open()) {
        acceptor.close(ec);
        std::println("[HttpSession] Acceptor closed successfully.");
    }
}

asio::awaitable<void> HttpSession::do_listen(){
    while (true) {
        auto [ec, socket] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));

        if (ec) {
            if (ec == asio::error::operation_aborted) {
                std::println("[HttpSession] Listen loop cancelled cleanly via shutdown.");
            } else {
                std::cerr << "[HttpSession Error] Accept failed: " << ec.message() << "\n";
            }
            break;
        }

        asio::co_spawn(
            acceptor.get_executor(),
            do_session(beast::tcp_stream(std::move(socket))),
            asio::detached
        );
    }
}

asio::awaitable<void> HttpSession::do_session(beast::tcp_stream stream){
    beast::flat_buffer buffer;
    try {
        while (true) {
            stream.expires_after(std::chrono::seconds(15));
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req);
            // 1. Проверяем, является ли HTTP-запрос попыткой апгрейда до WebSocket
            if (websocket::is_upgrade(req)) {
                if (req.target() == "/ws" || req.target() == "/")
                {
                    std::cout << "[WS] Upgrade requested on target: " << req.target() << "\n";

                    // Создаем WebSocket-стрим, перемещая в него наш tcp_stream
                    auto ws = std::make_shared<boost::beast::websocket::stream<beast::tcp_stream>>(std::move(stream));
                    ws->next_layer().expires_never();
                    // Выполняем асинхронный WebSocket-handshake
                    co_await ws->async_accept(req, asio::use_awaitable);

                    // Регистрируем сессию в бродкастере
                    broadcaster->add_session(ws);

                    // Запускаем фоновую корутину для чтения (актуально для контроля пингов и разрыва связи)
                    auto executor = co_await asio::this_coro::executor;
                    asio::co_spawn(executor, [ws]() -> asio::awaitable<void> {
                        boost::beast::flat_buffer ws_buffer;
                        try {
                            while (true) {
                                // Так как сервер работает ТОЛЬКО НА ОТПРАВКУ,
                                // все входящие сообщения от клиента мы просто читаем и выбрасываем.
                                co_await ws->async_read(ws_buffer, asio::use_awaitable);
                                ws_buffer.consume(ws_buffer.size());
                            }
                        } catch (boost::system::system_error const& se) {
                            // Сюда попадем, когда клиент закроет вкладку/браузер или пропадет сеть
                            std::cout << "[WS] Client disconnected or connection lost: " << se.what() << "\n";
                        }
                    }, asio::detached);

                    // Важно: выходим из функции do_session, так как сокет успешно передан WebSocket-клиенту
                    co_return;
                }


            }

            // 2. Если это обычный REST-запрос, обрабатываем как раньше
            bool keep_alive = req.keep_alive();

            auto msg = co_await handle_request(std::move(req));

            co_await beast::async_write(stream, std::move(msg));

            if (!keep_alive) break;
        }
    } catch (boost::system::system_error const& se) {
        if (se.code() != http::error::end_of_stream && se.code() != beast::error::timeout) {
            std::cerr << "[ERROR] " << se.what() << "\n";
        }
    }

    beast::error_code ec;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_send, ec);

}

asio::awaitable<http::message_generator> HttpSession::handle_request(http::request<http::string_body>&& req){
    std::cout << "[INFO] " << req.method_string() << " " << req.target() << "\n";

    if (req.method() == http::verb::options) {
        http::response<http::empty_body> res{http::status::no_content, req.version()};

        res.set(http::field::access_control_allow_origin, "*");
        res.set(http::field::access_control_allow_methods, "GET, POST, PATCH, DELETE, OPTIONS");
        res.set(http::field::access_control_allow_headers, "Content-Type, Authorization");
        res.set(http::field::access_control_max_age, "86400");

        res.keep_alive(req.keep_alive());
        res.prepare_payload();
        co_return res;
    }
    auto msg = co_await api_controller->route_request(std::move(req));
    co_return msg;
}
