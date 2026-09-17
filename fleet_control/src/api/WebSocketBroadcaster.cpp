
#include "../../include/api/WebSocketBroadcaster.hpp"

void WebSocketBroadcaster::add_session(std::shared_ptr<WsStream> ws){
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.push_back(ws);
    std::cout << "[WS] New client connected. Total clients: " << sessions_.size() << "\n";
}

void WebSocketBroadcaster::broadcast(const std::string& message){
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = sessions_.begin(); it != sessions_.end();) {
        auto ws = it->lock(); // Проверяем, жив ли объект
        if (!ws) {
            it = sessions_.erase(it);
            continue;
        }

        // Так как отправка асинхронная, запускаем её в контексте исполнителя сессии
        auto msg_shared = std::make_shared<std::string>(message);
        asio::post(ws->get_executor(), [ws, msg_shared]() {
            // Используем text() для отправки текстовых сообщений (JSON)
            ws->text(true);
            ws->async_write(asio::buffer(*msg_shared), [ws, msg_shared](beast::error_code ec, std::size_t) {
                if (ec) {
                    std::cerr << "[WS] Write error: " << ec.message() << "\n";
                    // Очистка произойдет при следующем broadcast или если сокет закроется
                }
            });
        });
        ++it;
    }
}
