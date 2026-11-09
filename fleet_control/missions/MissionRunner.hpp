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
class MissionRunner {
public:
    MissionRunner(asio::io_context& io_context, Services& services, MissionStore& missions, MapStore& maps);

    void start();   // стрим статусов миссий из dispatch, с переподключением
    void stop();

    // Миссия назначена (ASSIGNED), робот стоит на start_node: построить маршруты и отправить в dispatch
    void start_mission(Mission& mission, const std::string& start_node);

    // Отменить начатую миссию (ASSIGNED, RUNNING, PAUSED): CancelMission в dispatch, миссия → CANCELING.
    // dispatch её не знает — сразу CANCELED. Ошибка — dispatch недоступен
    asio::awaitable<grpc::Status> cancel(const std::string& mission_id);

private:
    using StreamRPC = agrpc::ClientRPC<&fms::dispatch::v1::FleetDispatch::Stub::PrepareAsyncStreamMissionStatus>;

    asio::awaitable<void> send(std::string mission_id, std::string start_node);
    asio::awaitable<void> watch_dispatch();
    void on_update(const fms::dispatch::v1::MissionStatusUpdate& update);
    void reconcile(const fms::dispatch::v1::MissionSnapshot& snapshot);

    // Миссия для dispatch. nullopt и why — маршрута нет
    std::optional<fms::v1::Mission> build(const Mission& mission, const std::string& start_node, std::string& why);
    void apply(Mission& mission, const fms::v1::MissionStatus& status);   // статус из dispatch → документ
    void back_to_queue(Mission& mission, const std::string& why);
    void finish(Mission& mission, const std::string& state, std::optional<MissionError> error);

    asio::io_context& io_context_;
    Services& services_;
    MissionStore& missions_;
    MapStore& maps_;
    std::set<std::string> sending_;   // AddMission в пути: сверка не должна вернуть их в очередь
    std::map<std::string, std::vector<std::vector<std::string>>> routes_;   // маршруты шагов (nodeId) — для UI, c2
    StreamRPC* stream_ = nullptr;     // текущий стрим — stop() его прерывает
    asio::steady_timer reconnect_timer_;
    bool stopped_ = false;
};

#endif //FLEETMANAGERSYSTEM_MISSIONRUNNER_HPP