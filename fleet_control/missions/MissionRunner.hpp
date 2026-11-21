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
#include "missions/RouteParts.hpp"
#include "missions/ExtensionCommand.hpp"
#include "missions/ControlCommandJournal.hpp"
#include "services/Services.hpp"
#include "fleet/AvailabilityTracker.hpp"
#include "traffic/FixedRouteGate.hpp"

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

    void configure_traffic(AvailabilityTracker& availability);

    bool traffic_ready() const;

    bool try_map_edit(const std::string& map);

    void end_map_edit(const std::string& map);
    // До вызова traffic обязан захватить hard-ресурсы нового base.
    // true — команда сохранена для отправки, НЕ подтверждение робота.
    // false — сейчас нельзя принять команду; invalid_argument — неверные данные.
    // При false/исключении вызывающий не должен слепо снимать резервы:
    // необходимо согласовать их с уже выданным base.
    bool queue_extension(
        const fms::dispatch::v1::ExtendMissionRequest& request);

    std::optional<ExtensionCommand::State> extension_state(const std::string& mission_id) const;

    // Миссия назначена (ASSIGNED), робот стоит на start_node: построить маршруты и отправить в dispatch
    void start_mission(Mission& mission, const std::string& start_node);

    // Отменить начатую миссию: сохраняем CANCELING до RPC.
    // Намерение отмены сохраняется; NOT_FOUND не доказывает отмену.
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

    // c2. Маршруты роботов для UI (GET /routes): что осталось проехать по текущему шагу и цели шагов
    struct RobotRoute {
        std::string robot_id;
        std::string map_id;
        std::string mission_id;
        int step_index = 0;

        // Оставшаяся разрешённая часть пути.
        std::vector<std::string> nodes;
        std::vector<std::string> edges;

        // Ещё не разрешённая часть пути.
        std::vector<std::string> planned_nodes;
        std::vector<std::string> planned_edges;

        std::vector<std::string> goals;
        std::string next_mission_id;
    };
    std::vector<RobotRoute> routes(const std::string& map_id);   // map_id "" — все карты

private:
    using StreamRPC = agrpc::ClientRPC<&fms::dispatch::v1::FleetDispatch::Stub::PrepareAsyncStreamMissionStatus>;

    // Маршрут отправленной миссии
    struct Plan {
        std::uint32_t traffic_version = 0;
        std::vector<std::vector<std::string>> routes;   // узлы маршрута каждого шага (nodeId)
        std::vector<std::vector<std::string>> edges;    // рёбра маршрута каждого шага (edgeId), для UI
        std::vector<std::string> planned_nodes;
        std::vector<std::string> planned_edges;
        std::vector<double> seconds;                     // время каждого шага по графу
        std::string last_node;                           // где робот сейчас (из стрима)
        std::vector<fms::v1::MoveProgress> progress;
    };
    // Следующая миссия робота
    struct Next {
        std::string mission_id;
        int64_t deadline_ms = 0;   // текущая не закончилась к этому времени — следующую обратно в очередь
    };

    asio::awaitable<void> send(std::string mission_id, std::string start_node);
    asio::awaitable<void> watch_dispatch();
    asio::awaitable<void> overdue_loop();
    asio::awaitable<void> restore(std::string mission_id);
    void request_restore(const std::string& mission_id);
    asio::awaitable<void> restore_loop();
    asio::awaitable<void> uncertain_loop();
    asio::awaitable<void> extension_loop();

    void observe_extension(const fms::v1::MissionStatus& status);

    void update_plan(Plan& plan, const fms::v1::MissionStatus& status);

    void on_update(const fms::dispatch::v1::MissionStatusUpdate& update);
    void reconcile(const fms::dispatch::v1::MissionSnapshot& snapshot);

    // Миссия для dispatch. nullopt и why — маршрута нет
    std::optional<fms::v1::Mission> build(const Mission& mission, const std::string& start_node, std::string& why);
    void apply(Mission& mission, const fms::v1::MissionStatus& status);   // статус из dispatch → документ
    void back_to_queue(Mission& mission, const std::string& why);
    // why — причина для истории (c3): нужна, когда нет error (отмена оператором)
    void finish(Mission& mission, const std::string& state, std::optional<MissionError> error,
                const std::string& why = "");
    // Текущая миссия робота закончилась: следующую — в dispatch (completed) или обратно в очередь
    void release_next(const std::string& robot_id, bool completed, const std::string& end_node);
    bool is_next(const std::string& mission_id) const;

    asio::io_context& io_context_;
    Services& services_;
    MissionStore& missions_;
    MapStore& maps_;
    std::set<std::string> sending_;          // AddMission в пути: сверка не должна вернуть их в очередь
    std::map<std::string, Plan> plans_;      // миссия → маршрут; есть у отправленных в dispatch
    // Пока маршрута нет, сохраняем последний статус из стрима.
    // GetMission не должен заменить его более старым статусом.
    std::map<std::string, fms::v1::MissionStatus> waiting_plans_;

    // Не запускаем несколько GetMission одновременно для одной миссии.
    std::set<std::string> restoring_;
    std::map<std::string, Next> next_;       // робот → его следующая миссия (c1b)
    StreamRPC* stream_ = nullptr;            // текущий стрим — stop() его прерывает
    asio::steady_timer reconnect_timer_;
    asio::steady_timer overdue_timer_;
    asio::steady_timer restore_timer_;
    std::set<std::string> uncertain_;
    asio::steady_timer uncertain_timer_;
    std::map<std::string, ExtensionCommand> extensions_;
    asio::steady_timer extension_timer_;

    void track_traffic(
    Mission& mission,
    Plan& plan,
    std::size_t step);

    asio::awaitable<void> traffic_loop();

    AvailabilityTracker* traffic_availability_ = nullptr;
    bool traffic_snapshot_ = false;
    bool traffic_fault_ = false;

    std::string traffic_error_;
    std::set<std::string> traffic_editing_;
    std::map<std::string, traffic::ProtectedRoute> traffic_routes_;

    traffic::FixedRouteGate traffic_gate_;
    asio::steady_timer traffic_timer_;


    ControlCommandJournal::Handle stage_command(const std::string& id, fms::database::v1::ControlCommandKind kind, const std::string& payload, const std::string& move_id = {},std::uint32_t revision = 0);

    asio::awaitable<grpc::Status> journaled_cancel(std::string id);
    asio::awaitable<void> journal_startup();
    std::uint32_t journal_attempt(const std::string& id) const;

    ControlCommandJournal command_journal_;
    std::map<std::string, std::uint32_t> journal_attempts_;
    std::set<std::string> add_rpc_started_;
    std::set<std::string> local_adds_;
    asio::steady_timer journal_timer_;
    bool journal_loaded_ = false;

    bool stopped_ = false;
};

#endif //FLEETMANAGERSYSTEM_MISSIONRUNNER_HPP