#include "MissionRunner.hpp"

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
    constexpr auto kOverduePeriod = std::chrono::seconds(5);
    constexpr auto kLoadPoll = std::chrono::milliseconds(100);
    constexpr auto kRestorePeriod = std::chrono::seconds(2);


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
    : io_context_(io_context),
      services_(services),
      missions_(missions),
      maps_(maps),
      reconnect_timer_(io_context),
      overdue_timer_(io_context),
      restore_timer_(io_context),
      uncertain_timer_(io_context) {}


void MissionRunner::start() {
    asio::co_spawn(io_context_, watch_dispatch(), log_failure);
    asio::co_spawn(io_context_, overdue_loop(), log_failure);
    asio::co_spawn(io_context_, restore_loop(), log_failure);
    asio::co_spawn(io_context_, uncertain_loop(), log_failure);
}

void MissionRunner::stop() {
    stopped_ = true;
    if (stream_) {
        stream_->context().TryCancel();   // read() вернёт false, корутина выйдет
    }
    reconnect_timer_.cancel();
    overdue_timer_.cancel();
    restore_timer_.cancel();
    uncertain_timer_.cancel();
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
        if (!mission) {
            // Отменили, пока миссия летела в dispatch (отмена её уже завершила), — а там она началась: остановить
            dpv1::CancelMissionRequest cancel;
            cancel.set_mission_id(mission_id);
            dpv1::CancelMissionResponse cancelled;
            co_await services_.call<&Dispatch::PrepareAsyncCancelMission>(*services_.dispatch, cancel, cancelled);
            co_return;
        }
        spdlog::info("[runner] mission {} sent to robot {}", mission_id, mission->robot_id);
        // Стрим статусов мог обогнать ответ на AddMission: тогда в миссии уже статус свежее этого
        if (mission->state == "ASSIGNED") {
            apply(*mission, response.status());
        }
        co_return;
    }
    if (!mission || mission->finished()) co_return;
    // Статус из стрима либо намерение отмены новее ответа отправки.
    if (mission->state != "ASSIGNED") co_return;
    switch (status.error_code()) {
        case grpc::StatusCode::NOT_FOUND:
        case grpc::StatusCode::FAILED_PRECONDITION:
            back_to_queue(*mission, status.error_message());
            break;
        case grpc::StatusCode::INVALID_ARGUMENT:
        case grpc::StatusCode::UNIMPLEMENTED:
            finish(*mission, "FAILED", MissionError{
                "INTERNAL", "dispatch rejected the mission: " + status.error_message()});
            break;
        default:
            // Не повторять AddMission: dispatch мог уже принять и закончить её.
            uncertain_.insert(mission_id);
            spdlog::warn("[runner] mission {}: send outcome unknown: {}",
                         mission_id, status.error_message());
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
    Plan plan;

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
        std::vector<std::string> edge_ids;
        for (uint32_t edge_index : route->edges) {
            const GraphEdge& edge = graph->edges[edge_index];
            auto* route_edge = move->add_edges();
            route_edge->set_edge_id(edge.id);
            if (edge.max_speed) {
                route_edge->set_maximum_speed(*edge.max_speed);
            }
            edge_ids.push_back(edge.id);
        }
        plan.routes.push_back(std::move(ids));
        plan.edges.push_back(std::move(edge_ids));
        plan.seconds.push_back(route->seconds);

        // Пока трафик не подключён, control разрешает весь шаг.
        // Начальный прогресс нужен и до первого сообщения dispatch.
        fms1::MoveProgress progress;
        progress.set_last_node_id(move->nodes(0).node_id());
        progress.set_last_node_index(0);
        progress.set_released_nodes(static_cast<std::uint32_t>(move->nodes_size()));
        progress.set_total_nodes(static_cast<std::uint32_t>(move->nodes_size()));

        plan.progress.push_back(std::move(progress));

        from = route->nodes.back();   // следующий шаг — оттуда, где закончился этот
    }
    plan.last_node = start_node;
    plans_[mission.id] = std::move(plan);
    return result;
}


void MissionRunner::update_plan(
    Plan& plan,
    const fms1::MissionStatus& status) {

    for (size_t i = 0; i < plan.progress.size(); ++i) {
        const std::string step_id = std::format("s{}", i);

        for (const auto& step : status.steps()) {
            if (step.node_id() != step_id || !step.has_move()) {
                continue;
            }

            // Копируем весь прогресс, включая сброс ожидания,
            // новую revision и текст отказа обновления.
            plan.progress[i] = step.move();

            if (status.current_node_id() == step_id) {
                const size_t at = step.move().last_node_index();

                if (i < plan.routes.size() &&
                    at < plan.routes[i].size()) {
                    plan.last_node = plan.routes[i][at];
                    }
            }

            break;
        }
    }
}

// ==================================================================== статусы

void MissionRunner::apply(Mission& mission, const fms1::MissionStatus& status) {

    if (mission.finished() || status.robot_id() != mission.robot_id) {
        return;
    }
    uncertain_.erase(mission.id);
    auto plan = plans_.find(mission.id);

    if (plan != plans_.end()) {
        update_plan(plan->second, status);
    } else {
        waiting_plans_[mission.id] = status;
    }

    const json before = mission_json(mission);
    const std::string old_state = mission.state;

    mission.state = without_prefix(fms1::MissionState_Name(status.state()), "MISSION_STATE_");
    // Старый RUNNING не отменяет намерение оператора.
    // Терминальный COMPLETED/FAILED/CANCELED сохраняем как фактический итог.
    if (old_state == "CANCELING" && !mission.finished()) {
        mission.state = "CANCELING";
    }

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
    if (!mission.finished()) {
        // В CANCELING dispatch переводит только по CancelMission, а его шлёт control по команде оператора.
        // Стрим может обогнать ответ на CancelMission — тогда причину для истории ставим здесь (c3)
        const bool canceling = mission.state == "CANCELING" && old_state != "CANCELING";
        missions_.changed(mission, canceling ? "canceled by operator" : "");
        return;
    }
    // Закончилась: следующая миссия робота — в dispatch или обратно в очередь
    const std::string robot_id = mission.robot_id;
    const bool completed = mission.state == "COMPLETED";
    std::string end_node;
    if (plan != plans_.end() && !plan->second.routes.empty()) {
        end_node = plan->second.routes.back().back();
        plans_.erase(plan);
    }
    waiting_plans_.erase(mission.id);
    missions_.changed(mission);   // завершённая уходит из памяти: mission дальше не трогаем
    release_next(robot_id, completed, end_node);
}

void MissionRunner::back_to_queue(Mission& mission, const std::string& why) {
    spdlog::warn("[runner] mission {} back to the queue ({}): {}", mission.id, mission.robot_id, why);
    mission.state = "QUEUED";
    mission.robot_id.clear();
    mission.assigned_at_ms.reset();

    plans_.erase(mission.id);
    waiting_plans_.erase(mission.id);
    // Не будим назначение сразу: робот, которого dispatch счёл занятым, тут же получил бы её снова.
    // Следующая попытка — по страховочному таймеру назначения (5 с) или по любому событию
    missions_.changed(mission, why);
}

void MissionRunner::finish(Mission& mission, const std::string& state, std::optional<MissionError> error, const std::string& why) {
    mission.state = state;
    mission.finished_at_ms = now_ms();
    mission.error = std::move(error);
    for (auto& step : mission.step_states) {
        if (step.state == "PENDING" || step.state == "RUNNING") {
            step.state = "CANCELED";
        }
    }
    spdlog::info("[runner] mission {} {}{}", mission.id, state, mission.error ? ": " + mission.error->message : "");
    const std::string robot_id = mission.robot_id;
    plans_.erase(mission.id);
    waiting_plans_.erase(mission.id);
    missions_.changed(mission, why);   // завершённая уходит из памяти: mission дальше не трогаем
    release_next(robot_id, false, "");
}

// ==================================================================== c1b: следующая миссия

std::vector<MissionRunner::SoonFree> MissionRunner::soon_free() {
    std::vector<SoonFree> result;
    for (Mission* mission : missions_.active()) {
        if (mission->state != "RUNNING" || !mission->current_step || next_.contains(mission->robot_id)) {
            continue;
        }
        auto plan = plans_.find(mission->id);
        if (plan == plans_.end()) {
            continue;   // маршрута нет (control перезапускался) — не знаем, сколько ему ехать
        }
        const size_t current = static_cast<size_t>(*mission->current_step);
        if (current >= mission->steps.size()) {
            continue;
        }

        if (current >= plan->second.progress.size()) {
            continue;
        }

        const auto& progress = plan->second.progress[current];

        // Пока нет оценки ожидания трафика, такого робота
        // не считаем предсказуемо "скоро свободным".
        if (progress.waiting_for_release() ||
            !progress.update_rejected().empty() ||
            progress.confirmed_revision() != progress.revision() ||
            progress.released_nodes() != progress.total_nodes()) {
            continue;
            }

        // Дальше только езда: ни у текущего шага, ни у следующих нет действий
        bool driving_only = true;
        for (size_t i = current; i < mission->steps.size(); ++i) {
            if (!mission->steps[i].actions.empty()) {
                driving_only = false;
            }
        }
        if (!driving_only) {
            continue;
        }

        // Осталось: от того, где робот сейчас, до конца текущего шага + все следующие шаги целиком
        const Graph* graph = maps_.find(mission->map_id);
        if (!graph) {
            continue;
        }
        const auto& route = plan->second.routes[current];
        auto from = graph->find_node(plan->second.last_node);
        auto to = graph->find_node(route.back());
        if (!from || !to) {
            continue;
        }
        std::vector<uint32_t> targets = {*to};
        auto rest = maps_.router(mission->map_id)->route(*from, targets);
        if (!rest) {
            continue;
        }
        double remaining = rest->seconds;
        for (size_t i = current + 1; i < plan->second.seconds.size(); ++i) {
            remaining += plan->second.seconds[i];
        }
        result.push_back({mission->robot_id, mission->map_id, plan->second.routes.back().back(), remaining});
    }
    return result;
}

void MissionRunner::set_next(Mission& mission, double remaining_seconds) {
    // Срок — две оценки плюс минута: короткие поездки не вылетают из-за пары секунд задержки
    const auto deadline = now_ms() + static_cast<int64_t>((2 * remaining_seconds + 60) * 1000);
    next_[mission.robot_id] = Next{mission.id, deadline};
    spdlog::info("[runner] mission {} is next for robot {} (it is busy for ~{:.0f} s more)", mission.id,
                 mission.robot_id, remaining_seconds);
}

void MissionRunner::release_next(const std::string& robot_id, bool completed, const std::string& end_node) {
    auto it = next_.find(robot_id);
    if (it == next_.end()) {
        return;
    }
    const std::string next_id = it->second.mission_id;
    next_.erase(it);
    Mission* next = missions_.find(next_id);
    if (!next || next->state != "ASSIGNED") {
        return;   // её уже отменили
    }
    if (completed && !end_node.empty()) {
        start_mission(*next, end_node);   // маршрут — от того места, где робот закончил
    } else {
        back_to_queue(*next, "the current mission of the robot did not complete");
    }
}

bool MissionRunner::is_next(const std::string& mission_id) const {
    return std::ranges::any_of(next_, [&](const auto& item) { return item.second.mission_id == mission_id; });
}

// Раз в 5 с: текущая миссия затянулась (робот застрял) — следующую отдать в очередь, пусть едет другой
asio::awaitable<void> MissionRunner::overdue_loop() {
    while (!stopped_) {
        overdue_timer_.expires_after(kOverduePeriod);
        co_await overdue_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        const int64_t now = now_ms();
        for (auto it = next_.begin(); it != next_.end();) {
            if (it->second.deadline_ms > now) {
                ++it;
                continue;
            }
            const std::string next_id = it->second.mission_id;
            it = next_.erase(it);
            if (Mission* next = missions_.find(next_id); next && next->state == "ASSIGNED") {
                back_to_queue(*next, "the current mission of the robot takes too long");
            }
        }
    }
}

// ==================================================================== стрим и сверка

asio::awaitable<void> MissionRunner::watch_dispatch() {
    // Сверка со снимком dispatch — только по миссиям и картам из базы: раньше времени она не найдёт своих
    // миссий — не заметит потерянных и не восстановит маршруты (T1.0b)
    while (!stopped_ && !(missions_.loaded() && maps_.loaded())) {
        reconnect_timer_.expires_after(kLoadPoll);
        co_await reconnect_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }

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

void MissionRunner::reconcile(const dpv1::MissionSnapshot& snapshot) {
    std::set<std::string> in_dispatch;

    for (const auto& status : snapshot.missions()) {
        in_dispatch.insert(status.mission_id());

        Mission* mission = missions_.find(status.mission_id());
        if (!mission) {
            continue;
        }

        apply(*mission, status);
        request_restore(status.mission_id());
    }

    int uncertain = 0;

    for (Mission* mission : missions_.active()) {
        if (mission->state == "QUEUED" ||
            in_dispatch.contains(mission->id) ||
            sending_.contains(mission->id) ||
            is_next(mission->id)) {
            continue;
            }

        // Отсутствие в снимке не запрещает позднюю доставку AddMission.
        uncertain_.insert(mission->id);
        ++uncertain;
    }

    spdlog::info(
        "[runner] connected to dispatch: {} missions there, {} unresolved",
        snapshot.missions_size(), uncertain);
}

// Маршруты — ровно те, что ушли роботу: из миссии в dispatch, а не построенные заново (карта могла измениться).
// Время шагов — по графу от начала до конца маршрута: нужно только для оценки «скоро свободен» (c1b)
asio::awaitable<void> MissionRunner::restore(std::string mission_id) {
    if (stopped_) {
        co_return;
    }

    dpv1::GetMissionRequest request;
    request.set_mission_id(mission_id);

    dpv1::GetMissionResponse response;
    grpc::Status status =
        co_await services_.call<&Dispatch::PrepareAsyncGetMission>(
            *services_.dispatch, request, response);

    if (stopped_) {
        co_return;
    }

    // За время RPC миссия могла завершиться, вернуться в очередь
    // или уже получить маршрут другим путём.
    Mission* mission = missions_.find(mission_id);
    if (!mission ||
        mission->finished() ||
        mission->state == "QUEUED" ||
        plans_.contains(mission_id)) {
        co_return;
    }

    auto latest = waiting_plans_.find(mission_id);
    if (latest == waiting_plans_.end()) {
        co_return;
    }

    if (!status.ok()) {
        // NOT_FOUND может означать, что миссия только что закончилась.
        // Её итог обрабатываем через стрим, здесь FAILED не ставим.
        if (status.error_code() != grpc::StatusCode::NOT_FOUND) {
            spdlog::warn(
                "[runner] mission {}: route restore failed, will retry: {}",
                mission_id, status.error_message());
        }
        co_return;
    }

    const auto& source = response.mission();

    if (source.mission_id() != mission_id ||
        source.robot_id() != mission->robot_id ||
        source.nodes_size() != static_cast<int>(mission->steps.size()) ||
        source.nodes_size() == 0) {
        spdlog::warn(
            "[runner] mission {}: unexpected mission from dispatch",
            mission_id);
        co_return;
    }

    const Graph* graph = maps_.find(mission->map_id);
    routing::Router* router = maps_.router(mission->map_id);

    Plan plan;

    for (int i = 0; i < source.nodes_size(); ++i) {
        const auto& node = source.nodes(i);

        // Control формирует линейные MoveStep s0, s1, ...
        // Пропускать неизвестный шаг нельзя: собьются индексы.
        if (node.id() != std::format("s{}", i) ||
            !node.has_move()) {
            spdlog::warn(
                "[runner] mission {}: unexpected step {}",
                mission_id, node.id());
            co_return;
        }

        const auto& move = node.move();

        if (move.nodes_size() == 0 ||
            move.edges_size() != move.nodes_size() - 1) {
            spdlog::warn(
                "[runner] mission {}: invalid route of step {}",
                mission_id, node.id());
            co_return;
        }

        const auto total =
            static_cast<std::uint32_t>(move.nodes_size());

        const auto released =
            move.has_released_nodes() ? move.released_nodes() : total;

        if (released == 0 || released > total) {
            spdlog::warn(
                "[runner] mission {}: invalid base of step {}",
                mission_id, node.id());
            co_return;
        }

        std::vector<std::string> ids;
        for (const auto& route_node : move.nodes()) {
            ids.push_back(route_node.node_id());
        }

        std::vector<std::string> edge_ids;
        for (const auto& edge : move.edges()) {
            edge_ids.push_back(edge.edge_id());
        }

        // Сохраняем существующий способ оценки времени для c1b.
        // Геометрия ниже остаётся именно из dispatch.
        double seconds = 0;
        if (graph && router) {
            auto from = graph->find_node(ids.front());
            auto to = graph->find_node(ids.back());

            if (from && to) {
                std::vector<uint32_t> targets = {*to};
                if (auto route = router->route(*from, targets)) {
                    seconds = route->seconds;
                }
            }
        }

        fms1::MoveProgress progress;
        progress.set_last_node_id(ids.front());
        progress.set_last_node_index(0);
        progress.set_released_nodes(released);
        progress.set_total_nodes(total);

        plan.routes.push_back(std::move(ids));
        plan.edges.push_back(std::move(edge_ids));
        plan.seconds.push_back(seconds);
        plan.progress.push_back(std::move(progress));
    }

    const int current = mission->current_step.value_or(0);
    if (current < 0 ||
        static_cast<size_t>(current) >= plan.routes.size()) {
        co_return;
    }

    plan.last_node = plan.routes[current].front();

    // Используем последний статус, полученный control из стрима.
    // response.status() намеренно не применяем: пока RPC летел,
    // робот мог пройти ещё несколько узлов или сменить шаг.
    update_plan(plan, latest->second);

    spdlog::info(
        "[runner] mission {}: route restored from dispatch, {} steps",
        mission_id, plan.routes.size());

    plans_[mission_id] = std::move(plan);
    waiting_plans_.erase(latest);
}

// ==================================================================== отмена

asio::awaitable<grpc::Status>
MissionRunner::cancel(const std::string& mission_id) {
    // Следующая миссия ещё не отправлялась — её можно отменить сразу.
    for (auto it = next_.begin(); it != next_.end(); ++it) {
        if (it->second.mission_id == mission_id) {
            next_.erase(it);

            if (Mission* mission = missions_.find(mission_id)) {
                finish(
                    *mission, "CANCELED", std::nullopt,
                    "canceled by operator");
            }

            co_return grpc::Status::OK;
        }
    }

    Mission* mission = missions_.find(mission_id);
    if (!mission || mission->finished()) {
        co_return grpc::Status::OK;
    }

    if (mission->state != "CANCELING") {
        mission->state = "CANCELING";
        missions_.changed(*mission, "canceled by operator");
    }

    // Намерение уже принято control. Доставку повторит uncertain_loop.
    dpv1::CancelMissionRequest request;
    request.set_mission_id(mission_id);

    dpv1::CancelMissionResponse response;
    co_await services_.call<&Dispatch::PrepareAsyncCancelMission>(
        *services_.dispatch, request, response);

    co_return grpc::Status::OK;
}

// ==================================================================== c2: маршруты для UI

std::vector<MissionRunner::RobotRoute>
MissionRunner::routes(const std::string& map_id) {
    std::vector<RobotRoute> result;

    for (Mission* mission : missions_.active()) {
        if (!map_id.empty() && mission->map_id != map_id) {
            continue;
        }

        auto plan = plans_.find(mission->id);
        if (plan == plans_.end()) {
            continue;
        }

        const int current_step = mission->current_step.value_or(0);
        if (current_step < 0) {
            continue;
        }

        const size_t current = static_cast<size_t>(current_step);
        const Plan& saved = plan->second;

        if (current >= saved.routes.size() ||
            current >= saved.edges.size() ||
            current >= saved.progress.size()) {
            continue;
            }

        auto parts = split_route(
            saved.routes[current],
            saved.edges[current],
            saved.progress[current]);

        if (!parts) {
            continue;
        }

        RobotRoute route;
        route.robot_id = mission->robot_id;
        route.map_id = mission->map_id;
        route.mission_id = mission->id;
        route.step_index = current_step;

        route.nodes = std::move(parts->released.nodes);
        route.edges = std::move(parts->released.edges);
        route.planned_nodes = std::move(parts->planned.nodes);
        route.planned_edges = std::move(parts->planned.edges);

        for (size_t i = current; i < mission->steps.size(); ++i) {
            route.goals.push_back(mission->steps[i].node_id);
        }

        if (auto next = next_.find(mission->robot_id);
            next != next_.end()) {
            route.next_mission_id = next->second.mission_id;
            }

        result.push_back(std::move(route));
    }

    return result;
}

void MissionRunner::request_restore(const std::string& mission_id) {
    if (stopped_ ||
        plans_.contains(mission_id) ||
        !waiting_plans_.contains(mission_id) ||
        sending_.contains(mission_id) ||
        is_next(mission_id)) {
        return;
        }

    Mission* mission = missions_.find(mission_id);
    if (!mission || mission->finished() || mission->state == "QUEUED") {
        return;
    }

    if (!restoring_.insert(mission_id).second) {
        return;
    }

    asio::co_spawn(
        io_context_,
        restore(mission_id),
        [this, mission_id](std::exception_ptr error) {
            // Освобождаем отметку и при исключении:
            // следующая попытка не должна заблокироваться навсегда.
            restoring_.erase(mission_id);
            log_failure(error);
        });
}

asio::awaitable<void> MissionRunner::restore_loop() {
    while (!stopped_) {
        restore_timer_.expires_after(kRestorePeriod);
        co_await restore_timer_.async_wait(asio::as_tuple(asio::use_awaitable));

        if (stopped_) {
            co_return;
        }

        if (!(missions_.loaded() && maps_.loaded())) {
            continue;
        }

        // Сначала копируем id: запущенная корутина может убрать
        // восстановленную миссию из waiting_plans_.
        std::vector<std::string> ids;
        ids.reserve(waiting_plans_.size());

        for (const auto& [id, status] : waiting_plans_) {
            ids.push_back(id);
        }

        for (const auto& id : ids) {
            request_restore(id);
        }
    }
}

asio::awaitable<void> MissionRunner::uncertain_loop() {
    std::string cursor;

    while (!stopped_) {
        uncertain_timer_.expires_after(std::chrono::milliseconds(500));
        co_await uncertain_timer_.async_wait(
            asio::as_tuple(asio::use_awaitable));

        if (stopped_) co_return;
        if (!(missions_.loaded() && maps_.loaded())) continue;

        // Одна сверка одновременно, без неограниченного fan-out.
        std::set<std::string> pending = uncertain_;

        for (const Mission* mission : missions_.active()) {
            if (mission->state == "CANCELING") {
                pending.insert(mission->id);
            }
        }

        for (auto it = pending.begin(); it != pending.end();) {
            Mission* mission = missions_.find(*it);

            if (!mission ||
                mission->finished() ||
                mission->state == "QUEUED") {
                uncertain_.erase(*it);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }

        if (pending.empty()) continue;

        auto selected = pending.upper_bound(cursor);
        if (selected == pending.end()) {
            selected = pending.begin();
        }

        const std::string id = *selected;
        cursor = id;

        if (sending_.contains(id) || is_next(id)) continue;

        dpv1::GetMissionRequest request;
        request.set_mission_id(id);
        dpv1::GetMissionResponse response;

        const auto status =
            co_await services_.call<&Dispatch::PrepareAsyncGetMission>(
                *services_.dispatch, request, response);

        if (stopped_) co_return;

        Mission* mission = missions_.find(id);
        if (!mission || mission->finished()) {
            uncertain_.erase(id);
            continue;
        }

        // Более свежий стрим уже разрешил неопределённость.
        // Ответ RPC не должен откатить полученный за время ожидания прогресс.
        if (status.ok() &&
            uncertain_.contains(id) &&
            response.status().mission_id() == id &&
            response.status().robot_id() == mission->robot_id &&
            !waiting_plans_.contains(id)) {
            apply(*mission, response.status());
            request_restore(id);
        }

        mission = missions_.find(id);
        if (!mission ||
            mission->finished() ||
            mission->state != "CANCELING") {
            continue;
        }

        dpv1::CancelMissionRequest cancel;
        cancel.set_mission_id(id);
        dpv1::CancelMissionResponse cancelled;

        co_await services_.call<&Dispatch::PrepareAsyncCancelMission>(
            *services_.dispatch, cancel, cancelled);

        // OK означает начало отмены, NOT_FOUND — неизвестный результат.
        // Терминальный статус получаем по стриму, включая COMPLETED.
    }
}