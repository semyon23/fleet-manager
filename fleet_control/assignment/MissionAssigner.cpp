#include "MissionAssigner.hpp"

#include <algorithm>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <limits>
#include <spdlog/spdlog.h>

using namespace boost::asio::experimental::awaitable_operators;

namespace {

constexpr auto kSafetyPeriod = std::chrono::seconds(5);
constexpr size_t kNearest = 5;   // сколько ближайших роботов искать на миссию — дальних волна не ищет
constexpr double kInf = std::numeric_limits<double>::infinity();

void fail_no_route(MissionStore& missions, Mission& mission, const std::string& message) {
    mission.state = "FAILED";
    mission.finished_at_ms = now_ms();
    mission.error = MissionError{"NO_ROUTE", message};
    for (auto& step : mission.step_states) {
        step.state = "CANCELED";
    }
    spdlog::warn("[assigner] mission {} failed: {}", mission.id, message);
    missions.changed(mission);   // завершённая уходит из памяти: mission дальше не трогаем
}

}  // namespace

MissionAssigner::MissionAssigner(asio::io_context& io_context, MissionStore& missions, AvailabilityTracker& availability,
                MapStore& maps, MissionRunner& runner)
    : io_context_(io_context),
      missions_(missions),
      availability_(availability),
      maps_(maps),
      runner_(runner),
      wakeup_(io_context, 1),
      timer_(io_context) {}

void MissionAssigner::start() {
    asio::co_spawn(io_context_, loop(), [](std::exception_ptr error) {
        if (!error) return;
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            spdlog::error("[assigner] coroutine failed: {}", e.what());
        }
    });
}

void MissionAssigner::stop() {
    stopped_ = true;
    wakeup_.close();
    timer_.cancel();
}

void MissionAssigner::wake() {
    wakeup_.try_send(boost::system::error_code{});
}

asio::awaitable<void> MissionAssigner::loop() {
    while (!stopped_) {
        timer_.expires_after(kSafetyPeriod);
        // Что раньше: сигнал «что-то изменилось» или 5 с. Вторую операцию || отменяет сам
        co_await (wakeup_.async_receive(asio::as_tuple(asio::use_awaitable)) ||
                  timer_.async_wait(asio::as_tuple(asio::use_awaitable)));
        if (!stopped_) {
            assign();
        }
    }
}


void MissionAssigner::assign() {
    if (!maps_.loaded() || !missions_.loaded()) {
        return;
    }
    std::vector<Mission*> queue = missions_.queue();
    if (queue.empty()) {
        return;
    }

    // Свободные: IDLE и без миссии в control (назначенной, но ещё не дошедшей до dispatch)
    const std::set<std::string> busy = missions_.busy_robots();
    std::vector<AvailabilityTracker::IdleRobot> free;
    for (auto& robot : availability_.idle_robots()) {
        if (!busy.contains(robot.robot_id)) {
            free.push_back(std::move(robot));
        }
    }
    // Скоро свободные (c1b): заканчивают миссию, дальше у них только езда, следующей ещё нет
    std::vector<MissionRunner::SoonFree> soon = runner_.soon_free();

    for (Mission* mission : queue) {
        if (free.empty() && soon.empty()) {
            break;   // назначать некому — остальные ждут следующего события
        }
        const Graph* graph = maps_.find(mission->map_id);
        if (!graph) {
            fail_no_route(missions_, *mission, "map " + mission->map_id + " no longer exists");
            continue;
        }
        auto first = graph->find_node(mission->steps.front().node_id);
        if (!first) {
            fail_no_route(missions_, *mission,
                          "node " + mission->steps.front().node_id + " is no longer on map " + mission->map_id);
            continue;
        }
        std::vector<uint32_t> targets = graph->arrival_nodes(*first);
        routing::Router* router = maps_.router(mission->map_id);
        auto suits = [&](const std::string& robot_id, const std::string& map_id) {
            return map_id == mission->map_id &&
                   (mission->requested_robot_id.empty() || robot_id == mission->requested_robot_id);
        };

        // ---- 1. Свободные: кто быстрее доедет до первого шага
        std::vector<size_t> free_candidates;   // индексы в free
        std::vector<uint32_t> free_nodes;      // их узлы в графе
        for (size_t i = 0; i < free.size(); ++i) {
            if (!suits(free[i].robot_id, free[i].map_id)) {
                continue;
            }
            if (auto node = graph->find_node(free[i].last_node_id)) {
                free_candidates.push_back(i);
                free_nodes.push_back(*node);
            }
        }
        double best_free_time = kInf;
        size_t best_free = 0;
        if (!free_nodes.empty()) {
            std::vector<double> times = router->times_to(targets, free_nodes, kNearest, kInf);
            best_free = static_cast<size_t>(std::ranges::min_element(times) - times.begin());
            best_free_time = times[best_free];
        }

        // ---- 2. Скоро свободные: остаток их миссии + путь от её конца до первого шага.
        // Волна — не дальше лучшего свободного: кому только от конца ехать дольше, тот точно проиграет
        std::vector<size_t> soon_candidates;   // индексы в soon
        std::vector<uint32_t> soon_nodes;      // где они освободятся
        for (size_t i = 0; i < soon.size(); ++i) {
            if (!suits(soon[i].robot_id, soon[i].map_id)) {
                continue;
            }
            if (auto node = graph->find_node(soon[i].end_node)) {
                soon_candidates.push_back(i);
                soon_nodes.push_back(*node);
            }
        }
        double best_soon_time = kInf;
        size_t best_soon = 0;
        if (!soon_nodes.empty()) {
            std::vector<double> times = router->times_to(targets, soon_nodes, soon_nodes.size(), best_free_time);
            for (size_t k = 0; k < times.size(); ++k) {
                const double total = times[k] + soon[soon_candidates[k]].remaining_seconds;
                if (total < best_soon_time) {
                    best_soon_time = total;
                    best_soon = k;
                }
            }
        }

        if (best_free_time == kInf && best_soon_time == kInf) {
            continue;   // никто до первого шага не доедет
        }
        mission->state = "ASSIGNED";
        mission->assigned_at_ms = now_ms();

        if (best_soon_time < best_free_time) {
            // Скоро свободный: миссия станет у него следующей и уйдёт в dispatch, когда текущая закончится
            const size_t chosen = soon_candidates[best_soon];
            const double remaining = soon[chosen].remaining_seconds;
            mission->robot_id = soon[chosen].robot_id;
            spdlog::info("[assigner] mission {} → robot {} after its current mission ({:.1f} s to the first step)",
                         mission->id, mission->robot_id, best_soon_time);
            soon.erase(soon.begin() + static_cast<std::ptrdiff_t>(chosen));
            missions_.changed(*mission);
            runner_.set_next(*mission, remaining);
        } else {
            const size_t chosen = free_candidates[best_free];
            mission->robot_id = free[chosen].robot_id;
            spdlog::info("[assigner] mission {} → robot {} ({:.1f} s to the first step)", mission->id,
                         mission->robot_id, best_free_time);
            const std::string start_node = free[chosen].last_node_id;   // до erase: потом free[chosen] — другой робот
            free.erase(free.begin() + static_cast<std::ptrdiff_t>(chosen));
            missions_.changed(*mission);
            runner_.start_mission(*mission, start_node);
        }
    }
}