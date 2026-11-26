#include "MissionRunner.hpp"
#include <limits>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <format>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/time_util.h>
#include <algorithm>
#include <type_traits>
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
      uncertain_timer_(io_context),
      extension_timer_(io_context),
      traffic_timer_(io_context),
      checkpoint_timer_(io_context),
      command_journal_(services),
      journal_timer_(io_context) {}


void MissionRunner::start() {
    asio::co_spawn(io_context_, watch_dispatch(), log_failure);
    asio::co_spawn(io_context_, overdue_loop(), log_failure);
    asio::co_spawn(io_context_, restore_loop(), log_failure);
    asio::co_spawn(io_context_, uncertain_loop(), log_failure);
    asio::co_spawn(io_context_, extension_loop(), log_failure);
    asio::co_spawn(io_context_, traffic_loop(), log_failure);
    asio::co_spawn(io_context_, journal_startup(), log_failure);
    asio::co_spawn(io_context_, checkpoint_loop(), log_failure);
    asio::co_spawn(io_context_, inventory_loop(), log_failure);
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
    extension_timer_.cancel();
    traffic_timer_.cancel();
    journal_timer_.cancel();
    checkpoint_timer_.cancel();
    inventory_timer_.cancel();
}

// ==================================================================== отправка в dispatch

void MissionRunner::start_mission(Mission& mission, const std::string& start_node) {
    if (!sending_.insert(mission.id).second) return;
    local_adds_.insert(mission.id);
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
    request.set_attempt(journal_attempt(mission_id));
    ControlCommandJournal::Handle saved_command;
    try {
        saved_command = stage_command(mission_id,
            fms::database::v1::CONTROL_COMMAND_KIND_ADD_MISSION, request.SerializeAsString());
    } catch (const std::exception& error) {
        sending_.erase(mission_id);
        traffic_fault_ = true;
        spdlog::error("[journal] cannot stage AddMission {}: {}", mission_id, error.what());
        co_return;
    }
    asio::steady_timer retry(io_context_);
    for (;;) {
        if (stopped_) { sending_.erase(mission_id); co_return; }
        mission = missions_.find(mission_id);
        if (!mission || mission->state != "ASSIGNED") {
            sending_.erase(mission_id);
            co_return;
        }
        const auto saved = co_await command_journal_.persist(saved_command);
        if (saved.ok()) break;
        retry.expires_after(std::chrono::seconds(1));
        co_await retry.async_wait(asio::as_tuple(asio::use_awaitable));
    }
    mission = missions_.find(mission_id);
    if (stopped_ || !mission || mission->state != "ASSIGNED") {
        sending_.erase(mission_id);
        co_return;
    }
    if (checkpoint_jobs_.contains(mission_id)) {
        sending_.erase(mission_id);
        co_return;
    }
    add_rpc_started_.insert(mission_id);
    dpv1::AddMissionResponse response;
    grpc::Status status = co_await services_.call<&Dispatch::PrepareAsyncAddMission>(*services_.dispatch, request, response);
    sending_.erase(mission_id);

    mission = missions_.find(mission_id);   // пока ждали — могли отменить: ссылки больше нет
    if (status.ok()) {
        // Из памяти исчезают только завершённые миссии. Не шлём новую
        // отмену по позднему успешному ответу уже завершённой миссии.
        if (!mission) co_return;
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
        case grpc::StatusCode::FAILED_PRECONDITION: {
            auto& attempt = journal_attempts_[mission_id];
            if (attempt == std::numeric_limits<std::uint32_t>::max()) {
                traffic_fault_ = true;
                spdlog::error("[journal] assignment attempt overflow: {}", mission_id);
                break;
            }
            const auto held = traffic_routes_.find(mission->robot_id);
            if (held != traffic_routes_.end() && held->second.nodes.size() == 1 &&
                held->second.key == mission_id + "/" + std::to_string(attempt) + "/s0") {
                // Add не принят. Стоящий робот остаётся занят через inventory,
                // но больше не принадлежит отвергнутой попытке этой миссии.
                traffic_routes_.erase(held);
            }
            add_rpc_started_.erase(mission_id);
            local_adds_.erase(mission_id);
            ++attempt; // Только доказанный отказ, НЕ тайм-аут.
            back_to_queue(*mission, status.error_message());
            break;
        }
        case grpc::StatusCode::INVALID_ARGUMENT:
        case grpc::StatusCode::UNIMPLEMENTED:
            finish(*mission, "FAILED", MissionError{
                "INTERNAL", "dispatch rejected the mission: " + status.error_message()});
            break;
        default:
            // Не повторять AddMission: dispatch мог уже принять и закончить её.
            uncertain_.insert(mission_id);
            queue_checkpoint(mission_id);
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
    plan.traffic_version = graph->version;

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

        move->set_released_nodes(1);

        plan.routes.push_back(std::move(ids));
        plan.edges.push_back(std::move(edge_ids));
        plan.seconds.push_back(route->seconds);

        // Пока трафик не подключён, control разрешает весь шаг.
        // Начальный прогресс нужен и до первого сообщения dispatch.
        fms1::MoveProgress progress;
        progress.set_last_node_id(move->nodes(0).node_id());
        progress.set_last_node_index(0);
        progress.set_released_nodes(1);
        progress.set_total_nodes(static_cast<std::uint32_t>(move->nodes_size()));

        plan.progress.push_back(std::move(progress));

        from = route->nodes.back();   // следующий шаг — оттуда, где закончился этот
    }
    plan.last_node = start_node;
    plans_[mission.id] = std::move(plan);
    return result;
}


void MissionRunner::update_plan(Plan& plan, const fms1::MissionStatus& status) {

    plan.status_instance = mission_stream_instance_;
    plan.step_attempts.assign(plan.progress.size(), 0);
    for (size_t i = 0; i < plan.progress.size(); ++i) {
        const std::string step_id = std::format("s{}", i);

        for (const auto& step : status.steps()) {
            if (step.node_id() != step_id || !step.has_move()) {
                continue;
            }

            // Копируем весь прогресс, включая сброс ожидания,
            // новую revision и текст отказа обновления.
            plan.progress[i] = step.move();
            plan.step_attempts[i] = step.attempts();

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

void MissionRunner::copy_status(Mission& mission, const fms1::MissionStatus& status) {
    const auto old_state = mission.state;
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
}

void MissionRunner::apply(Mission& mission, const fms1::MissionStatus& status, std::uint64_t final_version) {

    if (mission.finished() || status.robot_id() != mission.robot_id) {
        return;
    }

    if (status.mission_id() != mission.id) return;
    const bool final = status.state() == fms1::MISSION_STATE_COMPLETED ||
                       status.state() == fms1::MISSION_STATE_FAILED ||
                       status.state() == fms1::MISSION_STATE_CANCELED;
    if (!final_version && (final || checkpoint_jobs_.contains(mission.id))) {
        auto last = waiting_plans_.find(mission.id);
        if (last == waiting_plans_.end() ||
            TimeUtil::TimestampToNanoseconds(status.updated_at()) >=
                TimeUtil::TimestampToNanoseconds(last->second.updated_at()))
            waiting_plans_[mission.id] = status;
        queue_checkpoint(mission.id);
        return;
    }
    if (!final_version) observe_extension(status);

    uncertain_.erase(mission.id);
    auto plan = plans_.find(mission.id);

    if (!final_version && plan != plans_.end()) {
        update_plan(plan->second, status);
    } else if (!final_version) {
        waiting_plans_[mission.id] = status;
    }

    if (!final_version && plan != plans_.end()) {
        try {
            for (std::size_t i = 0;
                 i < plan->second.progress.size(); ++i) {
                const auto step_id = "s" + std::to_string(i);

                for (const auto& step : status.steps()) {
                    const auto held =
                        traffic_routes_.find(mission.robot_id);

                    const bool finishing_held =
                        step.state() == fms1::STEP_STATE_COMPLETED &&
                        held != traffic_routes_.end() &&
                        held->second.key ==
                            mission.id + "/" + std::to_string(journal_attempt(mission.id)) + "/" + step_id;

                    if (step.node_id() == step_id &&
                        step.has_move() &&
                        (finishing_held ||
                         status.current_node_id() == step_id)) {
                        track_traffic(mission, plan->second, i);
                         }
                }
                 }
        } catch (const std::exception& error) {
            traffic_fault_ = true;
            spdlog::error(
                "[traffic] grants blocked: {}", error.what());
        }
    }

    const json before = mission_json(mission);
    const std::string old_state = mission.state;

    copy_status(mission, status);

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
    // The checkpoint job retains ownership until storage acknowledges the final
    // and a current robot observation can take over the corridor.
    missions_.changed(mission, "", final_version);
}

void MissionRunner::back_to_queue(Mission& mission, const std::string& why) {
    spdlog::warn("[runner] mission {} back to the queue ({}): {}", mission.id, mission.robot_id, why);
    checkpoint_jobs_.erase(mission.id);
    uncertain_.erase(mission.id);
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
    command_journal_.forget(mission.id);
    if (!checkpoint_jobs_.contains(mission.id)) journal_attempts_.erase(mission.id);
    add_rpc_started_.erase(mission.id);
    local_adds_.erase(mission.id);
    missions_.changed(mission, why);   // завершённая уходит из памяти: mission дальше не трогаем
    release_next(robot_id, false, "");
}

// ==================================================================== c1b: следующая миссия

std::vector<MissionRunner::SoonFree> MissionRunner::soon_free() {
    std::vector<SoonFree> result{};
    // if (traffic_enabled_) {
    //     return result;
    // }
    //
    // for (Mission* mission : missions_.active()) {
    //     if (mission->state != "RUNNING" || !mission->current_step || next_.contains(mission->robot_id)) {
    //         continue;
    //     }
    //     auto plan = plans_.find(mission->id);
    //     if (plan == plans_.end()) {
    //         continue;   // маршрута нет (control перезапускался) — не знаем, сколько ему ехать
    //     }
    //     const size_t current = static_cast<size_t>(*mission->current_step);
    //     if (current >= mission->steps.size()) {
    //         continue;
    //     }
    //
    //     if (current >= plan->second.progress.size()) {
    //         continue;
    //     }
    //
    //     const auto& progress = plan->second.progress[current];
    //
    //     // Пока нет оценки ожидания трафика, такого робота
    //     // не считаем предсказуемо "скоро свободным".
    //     if (progress.waiting_for_release() ||
    //         !progress.update_rejected().empty() ||
    //         progress.confirmed_revision() != progress.revision() ||
    //         progress.released_nodes() != progress.total_nodes()) {
    //         continue;
    //         }
    //
    //     // Дальше только езда: ни у текущего шага, ни у следующих нет действий
    //     bool driving_only = true;
    //     for (size_t i = current; i < mission->steps.size(); ++i) {
    //         if (!mission->steps[i].actions.empty()) {
    //             driving_only = false;
    //         }
    //     }
    //     if (!driving_only) {
    //         continue;
    //     }
    //
    //     // Осталось: от того, где робот сейчас, до конца текущего шага + все следующие шаги целиком
    //     const Graph* graph = maps_.find(mission->map_id);
    //     if (!graph) {
    //         continue;
    //     }
    //     const auto& route = plan->second.routes[current];
    //     auto from = graph->find_node(plan->second.last_node);
    //     auto to = graph->find_node(route.back());
    //     if (!from || !to) {
    //         continue;
    //     }
    //     std::vector<uint32_t> targets = {*to};
    //     auto rest = maps_.router(mission->map_id)->route(*from, targets);
    //     if (!rest) {
    //         continue;
    //     }
    //     double remaining = rest->seconds;
    //     for (size_t i = current + 1; i < plan->second.seconds.size(); ++i) {
    //         remaining += plan->second.seconds[i];
    //     }
    //     result.push_back({mission->robot_id, mission->map_id, plan->second.routes.back().back(), remaining});
    // }
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
    while (!stopped_ &&
       !(missions_.loaded() && maps_.loaded() && journal_loaded_)) {
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
        if (traffic_snapshot_) {
            mission_stream_instance_.clear();
            invalidate_inventory();
        }
        grpc::Status status = co_await rpc.finish(asio::use_awaitable);
        stream_ = nullptr;
        if (traffic_snapshot_) {
            for (const auto* m : missions_.active()) {
                if (m->state != "QUEUED" && !is_next(m->id)) queue_checkpoint(m->id);
            }
            for (auto& [id, job] : checkpoint_jobs_) {
                ++job->epoch;
                waiting_plans_.erase(id);
            }
        }
        traffic_snapshot_ = false;
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

void MissionRunner::reconcile(
    const dpv1::MissionSnapshot& snapshot) {

    if (!traffic_snapshot_ || mission_stream_instance_ != snapshot.instance_id()) invalidate_inventory();
    mission_stream_instance_ = snapshot.instance_id();
    traffic_snapshot_ = true;

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
        waiting_plans_.erase(mission->id);
        queue_checkpoint(mission->id);
        ++uncertain;
    }

    spdlog::info(
        "[runner] connected to dispatch: {} missions there, {} unresolved",
        snapshot.missions_size(), uncertain);
}
// Маршруты — ровно те, что ушли роботу: из миссии в dispatch, а не построенные заново (карта могла измениться).
// Время шагов — по графу от начала до конца маршрута: нужно только для оценки «скоро свободен» (c1b)
asio::awaitable<void> MissionRunner::restore(std::string mission_id, Job job, std::uint64_t epoch,
                                           RecoveryCommands commands) {
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
        (!job && (plans_.contains(mission_id) || checkpoint_jobs_.contains(mission_id))) ||
        (job && (!checkpoint_current(mission_id, job, epoch) || !traffic_snapshot_))) {
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

    if (job && (response.status().mission_id() != mission_id ||
                response.status().robot_id() != mission->robot_id ||
                (response.status().state() != fms1::MISSION_STATE_RUNNING &&
                 response.status().state() != fms1::MISSION_STATE_PAUSED))) co_return;
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

    if (!graph) {
        co_return;
    }

    plan.traffic_version = graph ? graph->version : 0;

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
            const auto at = graph->find_node(route_node.node_id());

            if (!at ||
                route_node.map_id() != graph->map_id ||
                route_node.x() != graph->nodes[*at].x ||
                route_node.y() != graph->nodes[*at].y) {
                spdlog::error(
                    "[traffic] cannot restore route on changed map: {}",
                    mission_id);
                co_return;
                }

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

    const auto live = latest->second;
    update_plan(plan, live);
    if (job) {
        const auto previous = plans_.find(mission_id);
        if (previous != plans_.end()) {
            const auto& old = previous->second;
            if (plan.routes != old.routes || plan.edges != old.edges || plan.progress.size() != old.progress.size())
                co_return;
            for (std::size_t i = 0; i < plan.progress.size(); ++i) {
                const auto& p = plan.progress[i];
                const auto& prior = old.progress[i];
                if (p.revision() < prior.revision() || p.confirmed_revision() < prior.confirmed_revision() ||
                    p.last_node_index() < prior.last_node_index() || p.released_nodes() < prior.released_nodes())
                    co_return;
            }
        }
        if (!restore_journal_base(*mission, plan, live, commands)) co_return;
        if (job->assessment && job->proof_epoch == epoch &&
            job->proof_source_epoch == assessment_source_epoch_) {
            plan.restored_commands = std::move(commands);
        }
    }

    spdlog::info(
        "[runner] mission {}: route restored from dispatch, {} steps",
        mission_id, plan.routes.size());

    plans_[mission_id] = std::move(plan);
    waiting_plans_.erase(latest);

    if (job) {
        checkpoint_jobs_.erase(mission_id);
        apply(*mission, live);
    }
}

// ==================================================================== отмена

asio::awaitable<grpc::Status> MissionRunner::cancel(const std::string& mission_id) {
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

    co_return co_await journaled_cancel(mission_id);
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
        checkpoint_jobs_.contains(mission_id) ||
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
        if (!(journal_loaded_ && missions_.loaded() && maps_.loaded())) continue;

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
        if (checkpoint_jobs_.contains(id)) {
            // Only the operator's existing cancellation may be retried during recovery.
            if (auto* m = missions_.find(id); m && m->state == "CANCELING" && plans_.contains(id))
                co_await journaled_cancel(id);
            continue;
        }

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
            !checkpoint_jobs_.contains(id) && !waiting_plans_.contains(id)) {
            apply(*mission, response.status());
            request_restore(id);
        }

        mission = missions_.find(id);
        if (!mission ||
            mission->finished() ||
            mission->state != "CANCELING") {
            continue;
        }

        co_await journaled_cancel(id);

        // OK означает начало отмены, NOT_FOUND — неизвестный результат.
        // Терминальный статус получаем по стриму, включая COMPLETED.
    }
}

bool MissionRunner::queue_extension(
    const dpv1::ExtendMissionRequest& request) {

    Mission* mission = missions_.find(request.mission_id());

    if (!mission ||
        mission->state != "RUNNING" ||
        sending_.contains(mission->id) ||
        extensions_.contains(mission->id) ||
        !mission->current_step ||
        request.move_node_id() !=
            "s" + std::to_string(*mission->current_step)) {
        return false;
    }

    auto plan = plans_.find(mission->id);
    if (plan == plans_.end()) {
        return false;
    }

    const auto step =
        static_cast<std::size_t>(*mission->current_step);

    if (step >= plan->second.progress.size()) {
        return false;
    }

    ExtensionCommand command(
        request, plan->second.progress[step]);

    extensions_.emplace(mission->id, std::move(command));
    return true;
}

std::optional<ExtensionCommand::State>
MissionRunner::extension_state(
    const std::string& mission_id) const {

    auto found = extensions_.find(mission_id);
    if (found == extensions_.end()) {
        return std::nullopt;
    }

    return found->second.state();
}

void MissionRunner::observe_extension(
    const fms1::MissionStatus& status) {

    auto found = extensions_.find(status.mission_id());
    Mission* mission = missions_.find(status.mission_id());

    if (found == extensions_.end() ||
        !mission ||
        status.robot_id() != mission->robot_id) {
        return;
    }

    auto& command = found->second;

    for (const auto& step : status.steps()) {
        if (step.node_id() ==
                command.request().move_node_id() &&
            step.has_move()) {

            const auto before = command.state();
            command.observe(step.move());

            if (before != ExtensionCommand::State::Blocked &&
                command.state() == ExtensionCommand::State::Blocked) {
                spdlog::warn(
                    "[runner] extension {} blocked: {}",
                    status.mission_id(), command.reason());
            }

            break;
        }
    }
}

asio::awaitable<void> MissionRunner::extension_loop() {
    std::string cursor;

    while (!stopped_) {
        extension_timer_.expires_after(
            std::chrono::milliseconds(500));

        co_await extension_timer_.async_wait(
            asio::as_tuple(asio::use_awaitable));

        if (stopped_) co_return;
        if (extensions_.empty()) continue;

        if (!traffic_ready()) continue;

        auto entry = extensions_.upper_bound(cursor);
        if (entry == extensions_.end()) {
            entry = extensions_.begin();
        }

        const std::string id = entry->first;
        cursor = id;

        Mission* mission = missions_.find(id);

        if (!mission ||
            mission->finished() ||
            entry->second.state() ==
                ExtensionCommand::State::Confirmed) {
            extensions_.erase(entry);
            continue;
        }

        if (mission->state != "RUNNING" ||
            entry->second.state() ==
                ExtensionCommand::State::Blocked) {
            continue;
        }

        // Сначала сверяем миссию и шаг.
        // NOT_FOUND не снимает резервы.
        dpv1::GetMissionRequest get;
        get.set_mission_id(id);

        dpv1::GetMissionResponse got;

        const auto checked =
            co_await services_.call<
                &Dispatch::PrepareAsyncGetMission>(
                *services_.dispatch, get, got);

        if (stopped_) co_return;

        entry = extensions_.find(id);
        mission = missions_.find(id);

        if (entry == extensions_.end() ||
            !mission ||
            mission->state != "RUNNING" ||
            entry->second.state() ==
                ExtensionCommand::State::Blocked ||
            entry->second.state() ==
                ExtensionCommand::State::Confirmed) {
            continue;
        }

        if (!checked.ok() || !traffic_ready()) {
            continue;
        }

        if (got.status().mission_id() != id ||
            got.status().robot_id() != mission->robot_id ||
            got.status().state() != fms1::MISSION_STATE_RUNNING ||
            got.status().current_node_id() !=
                entry->second.request().move_node_id()) {

            entry->second.block(
                "mission or current step changed; "
                "reconcile before extending");

            spdlog::warn(
                "[runner] extension {} blocked: {}",
                id, entry->second.reason());

            continue;
        }

        observe_extension(got.status());

        entry = extensions_.find(id);

        if (entry->second.state() !=
            ExtensionCommand::State::DispatchPending) {
            continue;
        }

        // Копия переживает await; сохранённую команду не изменяем.
        const auto request = entry->second.request();
        ControlCommandJournal::Handle saved_command;
        try {
            saved_command = stage_command(id,
                fms::database::v1::CONTROL_COMMAND_KIND_EXTEND_MISSION,
                request.SerializeAsString(), request.move_node_id(), request.revision());
        } catch (const std::exception& error) {
            entry->second.block(error.what());
            spdlog::error("[journal] cannot stage extension {}: {}", id, error.what());
            continue;
        }
        const auto saved = co_await command_journal_.persist(saved_command);
        if (stopped_) co_return;
        if (!saved.ok()) continue;
        entry = extensions_.find(id);
        mission = missions_.find(id);
        if (!traffic_ready() || entry == extensions_.end() || !mission ||
            mission->state != "RUNNING" ||
            entry->second.state() != ExtensionCommand::State::DispatchPending ||
            entry->second.request().move_node_id() != request.move_node_id() ||
            entry->second.request().revision() != request.revision()) continue;
        dpv1::ExtendMissionResponse response;

        const auto sent =
            co_await services_.call<
                &Dispatch::PrepareAsyncExtendMission>(
                *services_.dispatch, request, response);

        if (stopped_) co_return;

        entry = extensions_.find(id);
        mission = missions_.find(id);

        if (entry == extensions_.end() ||
            !mission ||
            mission->state != "RUNNING") {
            continue;
        }

        if (sent.ok()) {
            entry->second.observe(response.progress());
        } else {
            switch (sent.error_code()) {
                case grpc::StatusCode::UNAVAILABLE:
                case grpc::StatusCode::DEADLINE_EXCEEDED:
                case grpc::StatusCode::CANCELLED:
                case grpc::StatusCode::UNKNOWN:
                case grpc::StatusCode::INTERNAL:
                    // Сверка и повтор только сохранённого запроса.
                    break;

                default:
                    entry->second.block(sent.error_message());
                    break;
            }
        }

        if (entry->second.state() ==
            ExtensionCommand::State::Blocked) {
            spdlog::warn(
                "[runner] extension {} blocked: {}",
                id, entry->second.reason());
        }
    }
}

void MissionRunner::configure_traffic(AvailabilityTracker& availability) {

    traffic_availability_ = &availability;
    availability.on_traffic_epoch = [this] { invalidate_inventory(); };
}

bool MissionRunner::traffic_ready() const {

    if (!journal_loaded_ ||
        traffic_fault_ ||
        !checkpoint_jobs_.empty() ||
        !traffic_availability_ ||
        !traffic_snapshot_ ||
        !maps_.loaded() ||
        !missions_.loaded() ||
        !traffic_availability_->traffic_inventory_ready() ||
        !traffic_editing_.empty()) {
        return false;
    }

    for (Mission* mission : missions_.active()) {
        if (mission->state != "QUEUED" &&
            !is_next(mission->id) &&
            !plans_.contains(mission->id)) {
            return false;
        }
    }

    return true;
}

bool MissionRunner::try_map_edit(const std::string& map) {
    if (!traffic_ready()) return false;

    for (Mission* mission : missions_.active()) {
        if (mission->map_id == map &&
            mission->state != "QUEUED") {
            return false;
        }
    }

    for (const auto& [robot, route] : traffic_routes_) {
        if (route.map == map &&
            route.reached + 1 < route.nodes.size()) {
            return false;
        }
    }

    return traffic_editing_.insert(map).second;
}

void MissionRunner::end_map_edit(const std::string& map) {
    traffic_editing_.erase(map);
}

void MissionRunner::track_traffic(
    Mission& mission,
    Plan& plan,
    std::size_t step) {

    if (step >= plan.progress.size() ||
        step >= plan.routes.size() ||
        step >= plan.edges.size()) {
        throw std::runtime_error("traffic step outside plan");
    }

    const auto* graph = maps_.find(mission.map_id);

    if (!graph || graph->version != plan.traffic_version) {
        throw std::runtime_error("traffic map version changed");
    }

    const auto& progress = plan.progress[step];
    const auto& nodes = plan.routes[step];
    const auto& edges = plan.edges[step];

    if (nodes.empty() ||
        edges.size() != nodes.size() - 1 ||
        progress.total_nodes() != nodes.size() ||
        progress.released_nodes() == 0 ||
        progress.released_nodes() > nodes.size() ||
        progress.last_node_index() >= progress.released_nodes() ||
        progress.last_node_id() !=
            nodes[progress.last_node_index()]) {
        throw std::runtime_error(
            "invalid indexed traffic progress");
    }

    traffic::ProtectedRoute next;
    next.robot = mission.robot_id;
    next.key = mission.id + "/" + std::to_string(journal_attempt(mission.id)) + "/s" + std::to_string(step);
    next.map = mission.map_id;
    next.version = graph->version;
    next.reached = progress.last_node_index();
    // Keep the approach to the final node until the robot actually stops on it.
    if (step + 1 == plan.routes.size() && nodes.size() > 1 && next.reached + 1 == nodes.size())
        --next.reached;

    std::size_t released = progress.released_nodes();
    auto old = traffic_routes_.find(mission.robot_id);

    if (old != traffic_routes_.end()) {
        const auto& held = old->second;

        if (held.key == next.key) {
            if (held.map != next.map ||
                held.version != next.version) {
                throw std::runtime_error(
                    "protected traffic map changed");
            }

            released = std::max(released, held.nodes.size());
            next.reached = std::max(next.reached, held.reached);
        } else {
            const auto start = graph->find_node(nodes.front());

            if (!start ||
                held.map != next.map ||
                held.version != next.version ||
                held.reached + 1 != held.nodes.size() ||
                held.nodes.back() != *start) {
                throw std::runtime_error(
                    "previous base has not been cleared");
            }
        }
    }

    if (released > nodes.size()) {
        throw std::runtime_error(
            "protected base exceeds route");
    }

    for (std::size_t i = 0; i < released; ++i) {
        const auto node = graph->find_node(nodes[i]);

        if (!node) {
            throw std::runtime_error(
                "traffic node missing from map");
        }

        next.nodes.push_back(*node);

        if (i != 0) {
            auto edge = std::find_if(
                graph->edges.begin(),
                graph->edges.end(),
                [&](const auto& edge) {
                    return edge.id == edges[i - 1];
                });

            if (edge == graph->edges.end() ||
                edge->from != next.nodes[i - 1] ||
                edge->to != next.nodes[i]) {
                throw std::runtime_error(
                    "traffic edge mismatch");
            }

            next.edges.push_back(
                static_cast<std::uint32_t>(
                    edge - graph->edges.begin()));
        }
    }

    if (old != traffic_routes_.end() &&
        old->second.key == next.key &&
        (!std::equal(
            old->second.nodes.begin(),
            old->second.nodes.end(),
            next.nodes.begin()) ||
         !std::equal(
            old->second.edges.begin(),
            old->second.edges.end(),
            next.edges.begin()))) {
        throw std::runtime_error(
            "protected traffic prefix changed");
    }

    traffic_routes_.insert_or_assign(
        mission.robot_id, std::move(next));
}

asio::awaitable<void> MissionRunner::traffic_loop() {
    while (!stopped_) {
        traffic_timer_.expires_after(
            std::chrono::milliseconds(200));

        co_await traffic_timer_.async_wait(
            asio::as_tuple(asio::use_awaitable));

        if (stopped_) co_return;
        if (!traffic_ready()) continue;

        try {
            const auto active = missions_.active();

            std::set<std::string> active_robots;
            for (auto* mission : active) {
                if (mission->state != "QUEUED") {
                    active_robots.insert(mission->robot_id);
                }
            }

            // Завершённый путь заменяется фактической позицией
            // стоящего робота из полного inventory.
            std::erase_if(
                traffic_routes_,
                [&](const auto& item) {
                    return
                        !active_robots.contains(item.first) &&
                        item.second.reached + 1 ==
                            item.second.nodes.size();
                });

            // Сначала учитываем ВСЕ активные разрешённые пути.
            for (Mission* mission : active) {
                if (mission->state == "QUEUED" ||
                    is_next(mission->id)) {
                    continue;
                }

                auto plan = plans_.find(mission->id);
                if (plan == plans_.end()) {
                    throw std::runtime_error(
                        "traffic plan missing");
                }

                const int step =
                    mission->current_step.value_or(0);

                if (step < 0) {
                    throw std::runtime_error(
                        "invalid traffic current step");
                }

                track_traffic(
                    *mission,
                    plan->second,
                    static_cast<std::size_t>(step));
            }

            std::vector<traffic::ProtectedRoute> held;
            for (const auto& [robot, route] : traffic_routes_) {
                held.push_back(route);
            }

            const auto occupants =
                traffic_availability_->traffic_occupants();

            auto ordered = active;
            const auto now = now_ms();

            std::sort(
                ordered.begin(),
                ordered.end(),
                [&](auto* a, auto* b) {
                    auto score = [&](auto* mission) {
                        const auto since =
                            mission->assigned_at_ms.value_or(
                                mission->created_at_ms);

                        return
                            static_cast<std::uint64_t>(
                                mission->priority) +
                            static_cast<std::uint64_t>(
                                std::max<std::int64_t>(
                                    0, now - since) / 30'000);
                    };

                    if (score(a) != score(b)) {
                        return score(a) > score(b);
                    }

                    const auto since_a =
                        a->assigned_at_ms.value_or(
                            a->created_at_ms);
                    const auto since_b =
                        b->assigned_at_ms.value_or(
                            b->created_at_ms);

                    if (since_a != since_b) {
                        return since_a < since_b;
                    }

                    return a->id < b->id;
                });

            // Не более одного нового запроса за цикл.
            for (Mission* mission : ordered) {
                if (mission->state != "RUNNING" ||
                    extensions_.contains(mission->id) ||
                    !mission->current_step) {
                    continue;
                }

                auto& plan = plans_.at(mission->id);
                const auto step =
                    static_cast<std::size_t>(
                        *mission->current_step);
                const auto& progress = plan.progress.at(step);

                if (progress.revision() !=
                        progress.confirmed_revision() ||
                    !progress.update_rejected().empty() ||
                    progress.released_nodes() ==
                        progress.total_nodes()) {
                    continue;
                }

                const auto* graph =
                    maps_.find(mission->map_id);

                auto wanted =
                    traffic_routes_.at(mission->robot_id);

                const auto old_size = wanted.nodes.size();

                for (std::size_t i = old_size;
                     i < plan.routes[step].size(); ++i) {
                    const auto node =
                        graph->find_node(plan.routes[step][i]);

                    if (!node) {
                        throw std::runtime_error(
                            "traffic node missing");
                    }

                    wanted.nodes.push_back(*node);

                    auto edge = std::find_if(
                        graph->edges.begin(),
                        graph->edges.end(),
                        [&](const auto& e) {
                            return e.id ==
                                plan.edges[step][i - 1];
                        });

                    if (edge == graph->edges.end()) {
                        throw std::runtime_error(
                            "traffic edge missing");
                    }

                    wanted.edges.push_back(
                        static_cast<std::uint32_t>(
                            edge - graph->edges.begin()));
                }

                const auto allowed = traffic_gate_.check(
                    *graph, occupants, held, wanted);

                if (!allowed.granted) continue;

                dpv1::ExtendMissionRequest request;
                request.set_mission_id(mission->id);
                request.set_move_node_id("s" + std::to_string(step));
                request.set_revision(progress.revision() + 1);
                request.set_released_nodes(progress.total_nodes());

                if (queue_extension(request)) {
                    // Зафиксировать резерв до следующего await.
                    static_assert(
                        std::is_nothrow_move_assignable_v<
                            traffic::ProtectedRoute>);

                    traffic_routes_.at(mission->robot_id) =
                        std::move(wanted);

                    spdlog::info(
                        "[traffic] mission {}: reserved {} "
                        "nodes before extension",
                        mission->id, progress.total_nodes());
                }

                break;
            }
        } catch (const std::exception& error) {
            traffic_fault_ = true;

            if (traffic_error_ != error.what()) {
                traffic_error_ = error.what();

                spdlog::error(
                    "[traffic] movement grants blocked: {}",
                    traffic_error_);
            }
        }
    }
}
std::uint32_t MissionRunner::journal_attempt(const std::string& id) const {
    auto found = journal_attempts_.find(id);
    return found == journal_attempts_.end() ? 0 : found->second;
}

ControlCommandJournal::Handle MissionRunner::stage_command(
    const std::string& id, fms::database::v1::ControlCommandKind kind,
    const std::string& payload, const std::string& move_id, std::uint32_t revision) {
    Mission* mission = missions_.find(id);
    auto plan = plans_.find(id);
    if (!mission || plan == plans_.end()) {
        throw std::runtime_error("journal requires mission and restored route");
    }
    const int step = mission->current_step.value_or(0);
    if (step < 0 || static_cast<std::size_t>(step) >= plan->second.progress.size()) {
        throw std::runtime_error("invalid step for journal snapshot");
    }
    const auto* graph = maps_.find(mission->map_id);
    if (!graph || graph->version != plan->second.traffic_version) {
        throw std::runtime_error("journal map version mismatch");
    }
    // AddMission ещё не ушёл: защищаем его старт и фиксируем поколение.
    track_traffic(*mission, plan->second, static_cast<std::size_t>(step));
    const auto& held = traffic_routes_.at(mission->robot_id);
    json progress = json::array();
    for (const auto& item : plan->second.progress) {
        std::string encoded;
        const auto status = google::protobuf::util::MessageToJsonString(item, &encoded);
        if (!status.ok()) throw std::runtime_error("cannot serialize move progress");
        progress.push_back(json::parse(encoded));
    }
    const auto attempt = journal_attempt(id);
    const bool cancelled_before_add = kind == fms::database::v1::CONTROL_COMMAND_KIND_CANCEL_MISSION &&
        local_adds_.contains(id) && !add_rpc_started_.contains(id);
    json snapshot = {
        {"version", 1}, {"attempt", attempt}, {"mission", mission_json(*mission)},
        {"mapVersion", graph->version}, {"routes", plan->second.routes},
        {"edges", plan->second.edges}, {"stepProgress", std::move(progress)},
        {"cancelBeforeAdd", cancelled_before_add},
        {"protectedBase", {
            {"robot", held.robot}, {"key", held.key}, {"map", held.map},
            {"version", held.version}, {"nodes", held.nodes},
            {"edges", held.edges}, {"reached", held.reached}}}
    };
    fms::database::v1::ControlCommandRecord record;
    // JSON tuple исключает неоднозначность разделителей в пользовательских id.
    record.set_command_id(json::array({id, attempt, static_cast<int>(kind), move_id, revision}).dump());
    record.set_robot_id(mission->robot_id);
    record.set_mission_id(id);
    record.set_map_id(mission->map_id);
    record.set_map_version(graph->version);
    record.set_kind(kind);
    record.set_move_node_id(move_id);
    record.set_revision(revision);
    record.set_attempt(attempt);
    record.set_request_proto(payload);
    record.set_recovery_version(1);
    record.set_recovery_json(snapshot.dump());
    plan->second.assessment.reset();
    auto entry = command_journal_.stage(std::move(record));
    if (auto it = checkpoint_jobs_.find(id); it != checkpoint_jobs_.end() &&
        it->second->last_command != entry->record.command_id()) {
        it->second->last_command = entry->record.command_id();
        ++it->second->epoch;
    }
    return entry;
}

asio::awaitable<grpc::Status> MissionRunner::journaled_cancel(std::string id) {
    auto* mission = missions_.find(id);
    if (!mission || mission->finished()) co_return grpc::Status::OK;
    if (mission->state != "CANCELING") {
        mission->state = "CANCELING";
        missions_.changed(*mission, "canceled by operator");
    }
    if (!journal_loaded_) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE, "command journal is loading"};
    }
    dpv1::CancelMissionRequest request;
    request.set_mission_id(id);
    ControlCommandJournal::Handle entry;
    try {
        entry = stage_command(id, fms::database::v1::CONTROL_COMMAND_KIND_CANCEL_MISSION,
                              request.SerializeAsString());
    } catch (const std::exception& error) {
        co_return grpc::Status{grpc::StatusCode::UNAVAILABLE, error.what()};
    }
    const auto saved = co_await command_journal_.persist(entry);
    if (!saved.ok()) co_return saved;
    if (stopped_) co_return grpc::Status{grpc::StatusCode::CANCELLED, "control stopping"};
    mission = missions_.find(id);
    if (!mission || mission->finished()) co_return grpc::Status::OK;
    const auto snapshot = json::parse(entry->record.recovery_json());
    if (snapshot.value("cancelBeforeAdd", false)) {
        if (add_rpc_started_.contains(id)) {
            traffic_fault_ = true;
            co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                                  "AddMission started despite cancellation"};
        }
        // Намерение отмены уже надёжно записано. AddMission не вызывался.
        queue_checkpoint(id);
        finish(*mission, "CANCELED", std::nullopt, "canceled before dispatch send");
        co_return grpc::Status::OK;
    }
    dpv1::CancelMissionResponse response;
    co_await services_.call<&Dispatch::PrepareAsyncCancelMission>(
        *services_.dispatch, request, response);
    // Результат исполнения отмены по-прежнему приходит из dispatch.
    co_return grpc::Status::OK;
}

asio::awaitable<void> MissionRunner::journal_startup() {
    using Storage = fms::database::v1::ControlStorage::Stub;
    std::string after;
    while (!stopped_) {
        if (!(missions_.loaded() && maps_.loaded())) {
            journal_timer_.expires_after(std::chrono::milliseconds(100));
            co_await journal_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
            continue;
        }
        fms::database::v1::ListOpenControlMissionsRequest request;
        request.set_after_mission_id(after);
        fms::database::v1::ListOpenControlMissionsResponse response;
        auto status = co_await services_.call<&Storage::PrepareAsyncListOpenControlMissions>(
            *services_.control, request, response);
        if (stopped_) co_return;
        if (status.ok()) {
            for (const auto& entry : response.missions()) {
                auto& attempt = journal_attempts_[entry.mission_id()];
                attempt = std::max(attempt, entry.last_attempt());
                queue_checkpoint(entry.mission_id());
            }
            if (!response.next_cursor().empty()) {
                after = response.next_cursor();
                continue;
            }
            for (const auto* m : missions_.active()) {
                if (m->state != "QUEUED" && !is_next(m->id)) queue_checkpoint(m->id);
            }
            journal_loaded_ = true;
            co_return;
        }
        journal_timer_.expires_after(std::chrono::seconds(2));
        co_await journal_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
    }
}


bool MissionRunner::restore_journal_base(
    Mission& mission, Plan& plan, const fms1::MissionStatus& status,
    const RecoveryCommands& commands) {
    namespace db = fms::database::v1;

    if (status.mission_id() != mission.id || status.robot_id() != mission.robot_id ||
        (status.state() != fms1::MISSION_STATE_RUNNING &&
         status.state() != fms1::MISSION_STATE_PAUSED) ||
        mission.state == "CANCELING") {
        return false;
    }

    int current_step = 0;
    while (current_step < static_cast<int>(plan.routes.size()) &&
           status.current_node_id() != "s" + std::to_string(current_step)) ++current_step;
    if (static_cast<std::size_t>(current_step) >= plan.routes.size()) {
        return false;
    }

    const auto* graph = maps_.find(mission.map_id);
    if (!graph || graph->version != plan.traffic_version) return false;

    const auto attempt = journal_attempt(mission.id);
    const db::ControlCommandRecord* add = nullptr;
    std::map<std::string, const db::ControlCommandRecord*> extensions;

    for (const auto& command : commands) {
        if (command.attempt() != attempt) continue;
        if (command.recovery_version() != 1 || command.robot_id() != mission.robot_id ||
            command.map_id() != mission.map_id || command.map_version() != graph->version) {
            return false;
        }

        switch (command.kind()) {
            case db::CONTROL_COMMAND_KIND_ADD_MISSION:
                if (add) return false;
                add = &command;
                break;
            case db::CONTROL_COMMAND_KIND_EXTEND_MISSION: {
                auto& latest = extensions[command.move_node_id()];
                if (!latest || latest->revision() < command.revision()) latest = &command;
                break;
            }
            default:
                // Включая отмену, исход которой надо сверять отдельно.
                return false;
        }
    }

    dpv1::AddMissionRequest original;
    if (!add || !original.ParseFromString(add->request_proto()) ||
        !original.has_mission()) {
        return false;
    }

    const auto& saved_mission = original.mission();
    if (saved_mission.mission_id() != mission.id ||
        saved_mission.robot_id() != mission.robot_id ||
        saved_mission.nodes_size() != static_cast<int>(plan.routes.size())) {
        return false;
    }

    try {
        const auto add_snapshot = json::parse(add->recovery_json());
        if (add_snapshot.at("routes") != json(plan.routes) ||
            add_snapshot.at("edges") != json(plan.edges)) {
            return false;
        }

        const db::ControlCommandRecord* current_command = nullptr;
        for (int i = 0; i < saved_mission.nodes_size(); ++i) {
            const auto step_id = std::format("s{}", i);
            const auto& saved_step = saved_mission.nodes(i);
            if (saved_step.id() != step_id || !saved_step.has_move()) return false;

            const auto& move = saved_step.move();
            const auto& nodes = plan.routes[i];
            const auto& edges = plan.edges[i];
            if (move.nodes_size() != static_cast<int>(nodes.size()) ||
                move.edges_size() != static_cast<int>(edges.size()) ||
                !move.has_released_nodes() || move.released_nodes() != 1) {
                return false;
            }
            for (int j = 0; j < move.nodes_size(); ++j) {
                if (move.nodes(j).node_id() != nodes[j]) return false;
            }
            for (int j = 0; j < move.edges_size(); ++j) {
                if (move.edges(j).edge_id() != edges[j]) return false;
            }

            auto extension = extensions.find(step_id);
            if (i > current_step) {
                if (extension != extensions.end()) return false;
                continue;
            }

            auto step = std::find_if(status.steps().begin(), status.steps().end(),
                [&](const auto& item) { return item.node_id() == step_id; });
            if (step == status.steps().end() || !step->has_move()) return false;

            const auto& progress = step->move();
            std::uint32_t revision = 0;
            std::uint32_t released = 1;
            if (extension != extensions.end()) {
                const auto& command = *extension->second;
                dpv1::ExtendMissionRequest request;
                if (!request.ParseFromString(command.request_proto()) ||
                    request.mission_id() != mission.id || request.move_node_id() != step_id ||
                    request.revision() == 0 || request.revision() != command.revision() ||
                    request.has_replace_tail() || !request.has_released_nodes()) {
                    return false;
                }
                revision = request.revision();
                released = request.released_nodes();
                if (i == current_step) current_command = &command;
                extensions.erase(extension);
            }

            if (released == 0 || released > nodes.size() ||
                progress.total_nodes() != nodes.size() ||
                progress.released_nodes() != released ||
                progress.revision() != revision || progress.confirmed_revision() != revision ||
                !progress.update_rejected().empty() ||
                progress.last_node_index() >= released ||
                progress.last_node_id() != nodes[progress.last_node_index()]) {
                return false;
            }
            if (i < current_step &&
                (step->state() != fms1::STEP_STATE_COMPLETED ||
                 progress.last_node_index() + 1 != nodes.size())) {
                return false;
            }
        }
        if (!extensions.empty()) return false;

        // В s0 есть снимок от Add. Для последующих шагов без Extend
        // исходная команда Add явно разрешает единственный стартовый узел.
        const auto* base_command = current_command ? current_command :
                                   (current_step == 0 ? add : nullptr);
        if (base_command) {
            const auto snapshot = json::parse(base_command->recovery_json());
            const auto& base = snapshot.at("protectedBase");
            const auto& progress = plan.progress.at(current_step);
            const auto& nodes = plan.routes.at(current_step);
            const auto& edges = plan.edges.at(current_step);
            const auto expected_key = mission.id + "/" + std::to_string(attempt)
                                    + "/s" + std::to_string(current_step);

            const auto saved_nodes = base.at("nodes").get<std::vector<std::uint32_t>>();
            const auto saved_edges = base.at("edges").get<std::vector<std::uint32_t>>();
            if (base.at("robot") != mission.robot_id || base.at("map") != mission.map_id ||
                base.at("version") != graph->version || base.at("key") != expected_key ||
                saved_nodes.empty() || saved_nodes.size() != progress.released_nodes() ||
                saved_edges.size() != saved_nodes.size() - 1 ||
                base.at("reached").get<std::uint64_t>() > progress.last_node_index()) {
                return false;
            }

            for (std::size_t i = 0; i < saved_nodes.size(); ++i) {
                const auto node = graph->find_node(nodes[i]);
                if (!node || *node != saved_nodes[i]) return false;
            }
            for (std::size_t i = 0; i < saved_edges.size(); ++i) {
                const auto index = saved_edges[i];
                if (index >= graph->edges.size()) return false;
                const auto& edge = graph->edges[index];
                if (edge.id != edges[i] || edge.from != saved_nodes[i] ||
                    edge.to != saved_nodes[i + 1]) {
                    return false;
                }
            }
        }

        // Every preceding step above has confirmed completion across its entire grant.
        // A warm control may still own its last observed prefix from an earlier step.
        auto old = traffic_routes_.find(mission.robot_id);
        decltype(traffic_routes_)::node_type previous;
        if (old != traffic_routes_.end()) {
            for (int i = 0; i < current_step; ++i) {
                if (old->second.key != mission.id + "/" + std::to_string(attempt) + "/s" + std::to_string(i))
                    continue;
                if (old->second.map != graph->map_id || old->second.version != graph->version ||
                    old->second.nodes.size() > plan.routes[i].size()) return false;
                for (std::size_t j = 0; j < old->second.nodes.size(); ++j) {
                    if (graph->find_node(plan.routes[i][j]) != old->second.nodes[j]) return false;
                    if (j && (old->second.edges.at(j-1) >= graph->edges.size() ||
                        graph->edges[old->second.edges[j-1]].id != plan.edges[i][j-1])) return false;
                }
                previous = traffic_routes_.extract(old);
                break;
            }
        }
        try {
            track_traffic(mission, plan, static_cast<std::size_t>(current_step));
        } catch (...) {
            if (!previous.empty()) traffic_routes_.insert(std::move(previous));
            throw;
        }
        return true;
    } catch (const std::exception& error) {
        spdlog::warn("[journal] cannot restore mission {}: {}", mission.id, error.what());
        return false;
    }
}