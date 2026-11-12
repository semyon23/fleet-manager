#ifndef FLEETMANAGERSYSTEM_MISSIONRUNNER_HPP
#define FLEETMANAGERSYSTEM_MISSIONRUNNER_HPP

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "maps/MapStore.hpp"
#include "missions/MissionStore.hpp"
#include "services/Services.hpp"

// Исполнение миссий (M-3c): назначенную миссию — в dispatch, статусы из dispatch — в документ миссии.
//
// Каждый шаг миссии — MoveStep "sN": маршрут по графу от того, где робот будет, до узла шага,
// весь отдан роботу сразу (трафика пока нет); действия шага — на последнем узле маршрута.
//
// c1b. Робот, которому осталось только ехать, может получить следующую миссию: она назначена ему (ASSIGNED),
// но уходит в dispatch, только когда текущая COMPLETED. Не дождалась — обратно в очередь.
class MissionRunner {
public:
    MissionRunner(asio::io_context& io_context, Services& services, MissionStore& missions, MapStore& maps);

    void start();   // стрим статусов миссий из dispatch и проверка затянувшихся
    void stop();

    // Миссия назначена (ASSIGNED), робот стоит на start_node: построить маршруты и отправить в dispatch
    void start_mission(Mission& mission, const std::string& start_node);

    // Отменить начатую миссию (ASSIGNED, RUNNING, PAUSED): CancelMission в dispatch, миссия → CANCELING.
    // dispatch её не знает или это следующая миссия робота — сразу CANCELED. Ошибка — dispatch недоступен
    asio::awaitable<grpc::Status> cancel(const std::string& mission_id);

    // c1b. Робот выполняет миссию, и дальше у него только езда: где и через сколько он освободится
    struct SoonFree {
        std::string robot_id;
        std::string map_id;
        std::string end_node;          // последний узел его миссии
        double remaining_seconds = 0;  // ехать до него от того места, где робот сейчас
    };
    std::vector<SoonFree> soon_free();

    // c1b. Миссия назначена роботу, который заканчивает другую: уйдёт в dispatch, когда та будет COMPLETED
    void set_next(Mission& mission, double remaining_seconds);

private:
    using StreamRPC = agrpc::ClientRPC<&fms::dispatch::v1::FleetDispatch::Stub::PrepareAsyncStreamMissionStatus>;

    // Маршрут отправленной миссии
    struct Plan {
        std::vector<std::vector<std::string>> routes;   // узлы маршрута каждого шага (nodeId) — и для UI, c2
        std::vector<double> seconds;                     // время каждого шага по графу
        std::string last_node;                           // где робот сейчас (из стрима)
    };
    // Следующая миссия робота
    struct Next {
        std::string mission_id;
        int64_t deadline_ms = 0;   // текущая не закончилась к этому времени — следующую обратно в очередь
    };

    asio::awaitable<void> send(std::string mission_id, std::string start_node);
    asio::awaitable<void> watch_dispatch();
    asio::awaitable<void> overdue_loop();
    void on_update(const fms::dispatch::v1::MissionStatusUpdate& update);
    void reconcile(const fms::dispatch::v1::MissionSnapshot& snapshot);

    // Миссия для dispatch. nullopt и why — маршрута нет
    std::optional<fms::v1::Mission> build(const Mission& mission, const std::string& start_node, std::string& why);
    void apply(Mission& mission, const fms::v1::MissionStatus& status);   // статус из dispatch → документ
    void back_to_queue(Mission& mission, const std::string& why);
    void finish(Mission& mission, const std::string& state, std::optional<MissionError> error);
    // Текущая миссия робота закончилась: следующую — в dispatch (completed) или обратно в очередь
    void release_next(const std::string& robot_id, bool completed, const std::string& end_node);
    bool is_next(const std::string& mission_id) const;

    asio::io_context& io_context_;
    Services& services_;
    MissionStore& missions_;
    MapStore& maps_;
    std::set<std::string> sending_;          // AddMission в пути: сверка не должна вернуть их в очередь
    std::map<std::string, Plan> plans_;      // миссия → маршрут; есть у отправленных в dispatch
    std::map<std::string, Next> next_;       // робот → его следующая миссия (c1b)
    StreamRPC* stream_ = nullptr;            // текущий стрим — stop() его прерывает
    asio::steady_timer reconnect_timer_;
    asio::steady_timer overdue_timer_;
    bool stopped_ = false;
};

#endif //FLEETMANAGERSYSTEM_MISSIONRUNNER_HPP