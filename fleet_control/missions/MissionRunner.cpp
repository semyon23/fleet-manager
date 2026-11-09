#include "MissionRunner.hpp"
#include "missions/MissionRunner.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <format>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/time_util.h>
#include <spdlog/spdlog.h>

namespace dpv1 = fms::dispatch::v1;
namespace fms1 = fms::v1;
using Dispatch = dpv1::FleetDispatch::Stub;
using google::protobuf::util::TimeUtil;
using nlohmann::json;

namespace {

constexpr auto kReconnectDelay = std::chrono::seconds(2);

void log_failure(std::exception_ptr error) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        spdlog::error("[runner] coroutine failed: {}", e.what());
    }
}

// "MISSION_STATE_RUNNING" → "RUNNING": имена proto-enum'ов совпадают с контрактом, без префикса
std::string without_prefix(const std::string& name, std::string_view prefix) {
    return name.starts_with(prefix) ? name.substr(prefix.size()) : name;
}

std::optional<int64_t> millis(const google::protobuf::Timestamp& time) {
    if (time.seconds() == 0 && time.nanos() == 0) {
        return std::nullopt;   // не задано
    }
    return TimeUtil::TimestampToMilliseconds(time);
}

// Действие шага из документа миссии (проверено MissionApi) → fms.v1.Action для dispatch
fms1::Action to_action(const json& item) {
    fms1::Action action;
    action.set_action_type(item.at("actionType").get<std::string>());

    std::string blocking = "HARD";   // по умолчанию; значение из factsheet робота — позже
    if (auto it = item.find("blockingType"); it != item.end() && it->is_string()) {
        blocking = it->get<std::string>();
    }
    fms1::BlockingType type = fms1::BLOCKING_TYPE_HARD;
    fms1::BlockingType_Parse("BLOCKING_TYPE_" + blocking, &type);
    action.set_blocking_type(type);

    if (auto it = item.find("descriptor"); it != item.end() && it->is_string()) {
        action.set_descriptor_(it->get<std::string>());
    }
    if (auto params = item.find("actionParameters"); params != item.end() && params->is_array()) {
        for (const auto& param : *params) {
            auto* parameter = action.add_parameters();
            parameter->set_key(param.at("key").get<std::string>());
            // Значение любого типа JSON → google.protobuf.Value
            (void)google::protobuf::util::JsonStringToMessage(param.at("value").dump(), parameter->mutable_value());
        }
    }
    return action;
}

}  // namespace

MissionRunner::MissionRunner(asio::io_context& io_context, Services& services, MissionStore& missions, MapStore& maps)
    : io_context_(io_context), services_(services), missions_(missions), maps_(maps), reconnect_timer_(io_context) {}

void MissionRunner::start() {
    asio::co_spawn(io_context_, watch_dispatch(), log_failure);
}

void MissionRunner::stop() {
    stopped_ = true;
    if (stream_) {
        stream_->context().TryCancel();   // read() вернёт false, корутина выйдет
    }
    reconnect_timer_.cancel();
}

// ==================================================================== отправка в dispatch

void MissionRunner::start_mission(Mission& mission, const std::string& start_node) {
    sending_.insert(mission.id);
    asio::co_spawn(io_context_, send(mission.id, start_node), log_failure);
}

asio::awaitable<void> MissionRunner::send(std::string mission_id, std::string start_node) {
    Mission* mission = missions_.find(mission_id);
    if (!mission || mission->state != "ASSIGNED") {
        sending_.erase(mission_id);
        co_return;
    }
    std::string why;
    std::optional<fms1::Mission> built = build(*mission, start_node, why);
    if (!built) {
        sending_.erase(mission_id);
        finish(*mission, "FAILED", MissionError{"NO_ROUTE", why});
        co_return;
    }

    dpv1::AddMissionRequest request;
    *request.mutable_mission() = std::move(*built);
    dpv1::AddMissionResponse response;
    grpc::Status status = co_await services_.call<&Dispatch::PrepareAsyncAddMission>(*services_.dispatch, request, response);
    sending_.erase(mission_id);

    mission = missions_.find(mission_id);   // пока ждали — могли отменить: ссылки больше нет
    if (status.ok()) {
        if (!mission || mission->state != "ASSIGNED") {
            // Отменили, пока миссия летела в dispatch, — а там она уже началась: остановить
            dpv1::CancelMissionRequest cancel;
            cancel.set_mission_id(mission_id);
            dpv1::CancelMissionResponse cancelled;
            co_await services_.call<&Dispatch::PrepareAsyncCancelMission>(*services_.dispatch, cancel, cancelled);
            co_return;
        }
        spdlog::info("[runner] mission {} sent to robot {}", mission_id, mission->robot_id);
        apply(*mission, response.status());
        co_return;
    }
    if (!mission) {
        co_return;
    }
    switch (status.error_code()) {
        case grpc::StatusCode::FAILED_PRECONDITION:   // робот уже занят или не на узле — dispatch знает свежее
        case grpc::StatusCode::UNAVAILABLE:
        case grpc::StatusCode::DEADLINE_EXCEEDED:
            back_to_queue(*mission, status.error_message());
            break;
        default:   // INVALID_ARGUMENT и прочее — ошибка в control
            finish(*mission, "FAILED", MissionError{"INTERNAL", "dispatch rejected the mission: " + status.error_message()});
    }
}

std::optional<fms1::Mission> MissionRunner::build(const Mission& mission, const std::string& start_node,
                                                  std::string& why) {
    const Graph* graph = maps_.find(mission.map_id);
    routing::Router* router = maps_.router(mission.map_id);
    if (!graph) {
        why = "map " + mission.map_id + " no longer exists";
        return std::nullopt;
    }
    auto from = graph->find_node(start_node);
    if (!from) {
        why = "robot node " + start_node + " is not on map " + mission.map_id;
        return std::nullopt;
    }

    fms1::Mission result;
    result.set_mission_id(mission.id);
    result.set_robot_id(mission.robot_id);
    std::vector<std::vector<std::string>> routes;

    for (size_t i = 0; i < mission.steps.size(); ++i) {
        const MissionStep& step = mission.steps[i];
        auto node = graph->find_node(step.node_id);
        if (!node) {
            why = std::format("step {}: node {} is no longer on map {}", i, step.node_id, mission.map_id);
            return std::nullopt;
        }
        auto route = router->route(*from, graph->arrival_nodes(*node));
        if (!route) {
            why = std::format("no route to step {} ({})", i, step.node_id);
            return std::nullopt;
        }

        auto* item = result.add_nodes();
        item->set_id(std::format("s{}", i));
        item->set_name(step.node_id);
        auto* move = item->mutable_move();
        std::vector<std::string> ids;
        for (size_t k = 0; k < route->nodes.size(); ++k) {
            const GraphNode& graph_node = graph->nodes[route->nodes[k]];
            auto* route_node = move->add_nodes();
            route_node->set_node_id(graph_node.id);
            route_node->set_x(graph_node.x);
            route_node->set_y(graph_node.y);
            route_node->set_map_id(mission.map_id);
            const bool last = k + 1 == route->nodes.size();
            if (last && graph_node.theta) {
                route_node->set_theta(*graph_node.theta);   // ориентация — только в конце шага, если задана у узла
            }
            if (last) {
                for (const auto& action : step.actions) {
                    *route_node->add_actions() = to_action(action);
                }
            }
            ids.push_back(graph_node.id);
        }
        for (uint32_t edge_index : route->edges) {
            const GraphEdge& edge = graph->edges[edge_index];
            auto* route_edge = move->add_edges();
            route_edge->set_edge_id(edge.id);
            if (edge.max_speed) {
                route_edge->set_maximum_speed(*edge.max_speed);
            }
        }
        routes.push_back(std::move(ids));
        from = route->nodes.back();   // следующий шаг — оттуда, где закончился этот
    }
    routes_[mission.id] = std::move(routes);
    return result;
}

// ==================================================================== статусы

void MissionRunner::apply(Mission& mission, const fms1::MissionStatus& status) {
    const json before = mission_json(mission);
    const std::string old_state = mission.state;

    mission.state = without_prefix(fms1::MissionState_Name(status.state()), "MISSION_STATE_");
    if (!mission.started_at_ms) {
        mission.started_at_ms = millis(status.started_at());
    }
    if (auto finished = millis(status.finished_at())) {
        mission.finished_at_ms = finished;
    }
    // current_node_id — "sN": номер шага
    mission.current_step.reset();
    if (status.current_node_id().starts_with('s')) {
        mission.current_step = std::stoi(status.current_node_id().substr(1));
    }
    for (const auto& step : status.steps()) {
        if (!step.node_id().starts_with('s')) {
            continue;
        }
        const auto index = static_cast<size_t>(std::stoi(step.node_id().substr(1)));
        if (index >= mission.step_states.size()) {
            continue;
        }
        StepState& state = mission.step_states[index];
        state.state = without_prefix(fms1::StepState_Name(step.state()), "STEP_STATE_");
        state.started_at_ms = millis(step.started_at());
        state.finished_at_ms = millis(step.finished_at());
        state.waiting_for_traffic = step.has_move() && step.move().waiting_for_release();
        state.actions = json::array();
        for (const auto& action : step.actions()) {
            state.actions.push_back({{"actionId", action.action_id()},
                                     {"actionType", action.action_type()},
                                     {"status", without_prefix(fms1::ActionStatus_Name(action.status()), "ACTION_STATUS_")}});
        }
    }
    if (status.has_error()) {
        mission.error = MissionError{without_prefix(fms1::MissionErrorCode_Name(status.error().code()), "MISSION_ERROR_CODE_"),
                                     status.error().message()};
    }

    if (mission_json(mission) == before) {
        return;   // dispatch прислал то же самое — писать нечего
    }
    if (mission.state != old_state) {
        spdlog::info("[runner] mission {}: {} → {}", mission.id, old_state, mission.state);
    }
    if (mission.finished()) {
        routes_.erase(mission.id);
    }
    missions_.changed(mission);   // завершённая уходит из памяти: mission дальше не трогаем
}

void MissionRunner::back_to_queue(Mission& mission, const std::string& why) {
    spdlog::warn("[runner] mission {} back to the queue ({}): {}", mission.id, mission.robot_id, why);
    mission.state = "QUEUED";
    mission.robot_id.clear();
    mission.assigned_at_ms.reset();
    routes_.erase(mission.id);
    // Не будим назначение сразу: робот, которого dispatch счёл занятым, тут же получил бы её снова.
    // Следующая попытка — по страховочному таймеру назначения (5 с) или по любому событию
    missions_.changed(mission);
}

void MissionRunner::finish(Mission& mission, const std::string& state, std::optional<MissionError> error) {
    mission.state = state;
    mission.finished_at_ms = now_ms();
    mission.error = std::move(error);
    for (auto& step : mission.step_states) {
        if (step.state == "PENDING" || step.state == "RUNNING") {
            step.state = "CANCELED";
        }
    }
    spdlog::info("[runner] mission {} {}{}", mission.id, state, mission.error ? ": " + mission.error->message : "");
    routes_.erase(mission.id);
    missions_.changed(mission);   // завершённая уходит из памяти: mission дальше не трогаем
}

asio::awaitable<void> MissionRunner::watch_dispatch() {
    while (!stopped_) {
        StreamRPC rpc{services_.grpc_context};
        stream_ = &rpc;
        dpv1::StreamMissionStatusRequest request;   // все роботы
        dpv1::MissionStatusUpdate update;
        if (co_await rpc.start(*services_.dispatch, request, asio::use_awaitable)) {
            while (co_await rpc.read(update, asio::use_awaitable)) {
                on_update(update);
            }
        }
        grpc::Status status = co_await rpc.finish(asio::use_awaitable);
        stream_ = nullptr;
        if (stopped_) {
            break;
        }
        spdlog::warn("[runner] dispatch mission stream closed: {}", status.error_message());
        reconnect_timer_.expires_after(kReconnectDelay);
        co_await reconnect_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }
}

void MissionRunner::on_update(const dpv1::MissionStatusUpdate& update) {
    if (update.has_snapshot()) {
        reconcile(update.snapshot());
    } else if (update.has_status()) {
        // Миссии не из control (тесты шлют AddMission в dispatch напрямую) — не наши, пропускаем
        if (Mission* mission = missions_.find(update.status().mission_id())) {
            apply(*mission, update.status());
        }
    }
}

// Первое сообщение стрима — что сейчас в dispatch: активные миссии и недавно завершённые.
// Так control узнаёт итог миссий, закончившихся, пока он лежал или стрим был оборван
void MissionRunner::reconcile(const dpv1::MissionSnapshot& snapshot) {
    std::set<std::string> in_dispatch;
    for (const auto& status : snapshot.missions()) {
        in_dispatch.insert(status.mission_id());
        if (Mission* mission = missions_.find(status.mission_id())) {
            apply(*mission, status);
        }
    }
    int lost = 0;
    for (Mission* mission : missions_.active()) {
        if (mission->state == "QUEUED" || in_dispatch.contains(mission->id) || sending_.contains(mission->id)) {
            continue;
        }
        if (mission->state == "ASSIGNED") {
            back_to_queue(*mission, "not in dispatch: AddMission did not get there");
        } else {
            finish(*mission, "FAILED", MissionError{"INTERNAL", "mission lost while fms restarted"});
        }
        ++lost;
    }
    spdlog::info("[runner] connected to dispatch: {} missions there, {} lost", snapshot.missions_size(), lost);
}

// ==================================================================== отмена

asio::awaitable<grpc::Status> MissionRunner::cancel(const std::string& mission_id) {
    dpv1::CancelMissionRequest request;
    request.set_mission_id(mission_id);
    dpv1::CancelMissionResponse response;
    grpc::Status status = co_await services_.call<&Dispatch::PrepareAsyncCancelMission>(*services_.dispatch, request, response);

    Mission* mission = missions_.find(mission_id);
    if (!mission) {
        co_return grpc::Status::OK;   // закончилась сама, пока ждали
    }
    if (status.ok()) {
        if (mission->state != "CANCELING") {
            mission->state = "CANCELING";   // CANCELED придёт по стриму, когда робот подтвердит
            missions_.changed(*mission);
        }
        co_return grpc::Status::OK;
    }
    if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
        // dispatch её не знает: AddMission ещё в пути (дойдёт — send() сам её остановит) или потерян
        finish(*mission, "CANCELED", std::nullopt);
        co_return grpc::Status::OK;
    }
    co_return status;   // dispatch недоступен
}
