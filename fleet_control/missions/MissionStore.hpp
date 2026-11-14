#ifndef FLEETMANAGERSYSTEM_MISSIONSTORE_HPP
#define FLEETMANAGERSYSTEM_MISSIONSTORE_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "missions/Mission.hpp"
#include "services/Services.hpp"

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

class MissionStore {
public:
    MissionStore(asio::io_context& io_context, Services& services);

    void start();   // загрузить незавершённые миссии из базы и начать писать изменения
    void stop();
    bool loaded() const { return loaded_; }

    // Новая миссия. Ответ — когда она записана в базу: оператору нельзя сказать «принято»,
    // а после перезапуска миссию потерять. Не записалась — её нет и в памяти
    asio::awaitable<grpc::Status> create(Mission mission);

    // Миссия по id — активная из памяти или завершённая из базы. Нет нигде — OK и nullopt
    asio::awaitable<std::pair<grpc::Status, std::optional<Mission>>> get(const std::string& id);

    Mission* find(const std::string& id);   // только активные; nullptr — нет или уже завершена
    std::vector<Mission*> queue();          // QUEUED по порядку: приоритет ↓, createdAt ↑, id ↑

    // Миссию поменяли (состояние, робот, шаги): updatedAt, события истории (c3), запись в базу через очередь.
    // why — почему сменилось состояние, попадёт в событие. Завершённая уходит из памяти — ссылка mission
    // после вызова недействительна
    nlohmann::json changed(Mission& mission, const std::string& why = "");

    std::vector<Mission*> active();   // все незавершённые: для сверки с dispatch
    std::set<std::string> busy_robots() const;   // роботы активных миссий: им новую не давать
    // Очередь могла измениться: новая миссия, миссия закончилась. M-3c: будит распределение
    std::function<void()> on_queue_change;

private:
    asio::awaitable<void> load();
    asio::awaitable<void> write_loop();

    using Signal = asio::experimental::channel<void(boost::system::error_code)>;

    asio::io_context& io_context_;
    Services& services_;
    std::unordered_map<std::string, Mission> active_;

    // Ещё не записано в базу: последняя версия документа и всё, что с миссией случилось с прошлой записи
    struct Pending {
        fms::database::v1::MissionRecord record;
        std::vector<fms::database::v1::MissionEvent> events;
    };
    std::unordered_map<std::string, Mission> last_;   // прошлая версия активной миссии — с ней сравнивает changed
    std::map<std::string, Pending> pending_;
    Signal wakeup_;                     // «в pending_ есть что писать»
    asio::steady_timer retry_timer_;    // пауза, если база недоступна
    bool loaded_ = false;
    bool stopped_ = false;
};

#endif //FLEETMANAGERSYSTEM_MISSIONSTORE_HPP
