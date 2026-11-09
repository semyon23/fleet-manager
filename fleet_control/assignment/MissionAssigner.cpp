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

MissionAssigner::MissionAssigner(asio::io_context& io_context, MissionStore& missions,
                                 AvailabilityTracker& availability, MapStore& maps)
    : io_context_(io_context),
      missions_(missions),
      availability_(availability),
      maps_(maps),
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


    const std::set<std::string> busy = missions_.busy_robots();
    std::vector<AvailabilityTracker::IdleRobot> free;
    for (auto& robot : availability_.idle_robots()) {
        if (!busy.contains(robot.robot_id)) {
            free.push_back(std::move(robot));
        }
    }

    for (Mission* mission : queue) {
        if (free.empty()) {
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

        // Кандидаты: свободные на карте миссии; если робота попросили — только он
        std::vector<size_t> candidates;   // индексы в free
        std::vector<uint32_t> nodes;      // их узлы в графе
        for (size_t i = 0; i < free.size(); ++i) {
            if (free[i].map_id != mission->map_id) {
                continue;
            }
            if (!mission->requested_robot_id.empty() && free[i].robot_id != mission->requested_robot_id) {
                continue;
            }
            if (auto node = graph->find_node(free[i].last_node_id)) {
                candidates.push_back(i);
                nodes.push_back(*node);
            }
        }
        if (nodes.empty()) {
            continue;
        }

        // Одна обратная волна от первого шага: время каждого кандидата по дорогам карты
        std::vector<double> times = maps_.router(mission->map_id)->times_to(targets, nodes, kNearest, kInf);
        const auto best = static_cast<size_t>(std::ranges::min_element(times) - times.begin());
        if (times[best] == kInf) {
            continue;   // никто из свободных до первого шага не доедет
        }

        const size_t chosen = candidates[best];
        mission->state = "ASSIGNED";
        mission->robot_id = free[chosen].robot_id;
        mission->assigned_at_ms = now_ms();
        spdlog::info("[assigner] mission {} → robot {} ({:.1f} s to the first step)", mission->id,
                     mission->robot_id, times[best]);
        const std::string start_node = free[chosen].last_node_id;
        free.erase(free.begin() + static_cast<std::ptrdiff_t>(chosen));
        missions_.changed(*mission);
        if (on_assigned) {
            on_assigned(*mission, start_node);
        }
    }
}