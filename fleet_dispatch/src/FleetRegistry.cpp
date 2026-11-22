#include "../include/FleetRegistry.hpp"
#include "spdlog/spdlog.h"
#include "../include/VdaTopics.hpp"
#include <google/protobuf/util/time_util.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>


FleetRegistry::FleetRegistry(asio::io_context& io_context, Outbox& outbox, RobotStatusHub& status_hub, MissionStatusHub& mission_hub,
                  DatabaseWriter& database)
    : io_context_(io_context), outbox_(outbox),  status_hub_(status_hub), mission_hub_(mission_hub), database_(database)  {}

AddResult FleetRegistry::add(const std::string& robot_id, const std::string& manufacturer,
                             const std::string& serial_number) {
    if (RobotSession* existing = find(robot_id)) {
        if (existing->manufacturer() == manufacturer && existing->serial_number() == serial_number) {
            return AddResult::AlreadyExists;
        }
        return AddResult::Conflict;
    }
    if (find_by_vda(manufacturer, serial_number)) {
        return AddResult::Conflict;
    }
    auto session = std::make_shared<RobotSession>(io_context_, outbox_, status_hub_, mission_hub_, database_, robot_id, manufacturer, serial_number);
    sessions_[robot_id] = session;
    session->start();
    spdlog::info("[registry] robot {} added, total {}", robot_id, sessions_.size());
    return AddResult::Added;
}

asio::awaitable<void> FleetRegistry::restore(StartupInventory inventory) {
    for (const auto& record : inventory.snapshot.robots()) {
        if (stopping_) co_return;

        const auto& s = record.spec();
        auto session = std::make_shared<RobotSession>(
            io_context_, outbox_, status_hub_, mission_hub_, database_,
            s.robot_id(), s.manufacturer(), s.serial_number());

        sessions_.emplace(s.robot_id(), session);

        std::string mission;
        auto it = inventory.occupied.find(s.robot_id());
        if (it != inventory.occupied.end()) mission = it->second;

        co_await session->start_restored(record, std::move(mission));
    }

    if (!stopping_) {
        inventory_.emplace(std::move(inventory));
        start_probes();
    }
}


void FleetRegistry::start_probes() {
    const auto& inv = *inventory_;
    using Execution = RecoveryProbe::Execution;

    std::map<std::string, const Execution*> heads, grants;
    for (const auto& e : inv.snapshot.active_missions()) {
        heads.emplace(e.mission().robot_id(), &e);
    }
    for (const auto& e : inv.snapshot.last_grants()) {
        grants.emplace(e.mission().mission_id(), &e);
    }

    for (const auto& record : inv.snapshot.robots()) {
        const auto& spec = record.spec();
        const auto& id = spec.robot_id();
        if (inv.deleting.contains(id)) continue;

        auto& p = probes_[id];
        p.robot = &spec;

        const auto it = heads.find(id);
        if (inv.consistent && it != heads.end()) {
            const auto& e = *it->second;
            for (const auto& n : e.mission().nodes()) {
                if (n.id() != e.status().current_node_id() || !n.has_move() ||
                    e.status().state() != fms::v1::MISSION_STATE_RUNNING) {
                    continue;
                }
                if (auto g = grants.find(e.mission().mission_id()); g != grants.end()) {
                    p.head = &e;
                    p.grant = g->second;
                }
            }
        }

        find(id)->observe_recovery([this, id](const auto* state) {
            return observe(id, state);
        });
        queue_.add(id, ProbeQueue::Clock::now());
    }

    pump_probes();
}

void FleetRegistry::pump_probes() {
    if (stopping_ || timer_armed_) return;

    const auto now = ProbeQueue::Clock::now();
    for (const auto& id : queue_.take(now)) {
        auto& p = probes_.at(id);
        p.token = "recovery-" + boost::uuids::to_string(boost::uuids::random_generator()());
        if (!find(id)->request_probe(p.token)) queue_.retry(id, now);
    }

    if (queue_.empty()) return;
    timer_armed_ = true;
    probe_timer_.expires_after(std::chrono::milliseconds(200));
    probe_timer_.async_wait([this](boost::system::error_code ec) {
        timer_armed_ = false;
        if (!ec) pump_probes();
    });
}

bool FleetRegistry::observe(const std::string& id, const vda5050::State* state) {
    using D = move_recovery::Decision;
    auto& p = probes_.at(id);
    const auto now = ProbeQueue::Clock::now();

    if (p.decision && p.decision->decision == D::Conflict) return false;

    auto retry = [&] {
        p.reset();
        queue_.retry(id, now);
        pump_probes();
    };

    if (!state) {
        retry();
        return false;
    }

    if (!queue_.live(id, now) || !p.acknowledged(*state)) return false;
    queue_.remove(id);

    if (!p.head) {
        if (!inventory_->occupied.contains(id) &&
            (state->driving || !vda5050::is_idle(*state))) {
            retry();
            return false;
        }
        return true;
    }

    auto* session = find(id);
    if (!session) return false;

    // После потери связи новый stateRequest подтверждает свежесть.
    // Существующий экземпляр миссии сам проверит маршрут и текущий прогресс.
    if (session->mission()) return true;

    auto restored = ActiveMission::restore_move(
        *p.robot, *p.head, *p.grant, p.token, *state, now,
        google::protobuf::util::TimeUtil::GetCurrentTime());

    if (restored && session->attach_move(std::move(*restored),
                                         p.head->checkpoint_version())) {
        spdlog::info("[recovery] robot={} mission={} result=Attach reason=matched",
                     id, p.head->mission().mission_id());
        return true;
    }

    // На успешном пути сверка выполняется один раз, внутри restore_move().
    // Здесь различаем отстающий update и постоянное противоречие.
    auto result = move_recovery::reconcile(*p.robot, *p.head, *p.grant, p.token, state);
    if (result.decision == D::Wait) {
        retry();
        return false;
    }
    if (result.decision == D::Attach) {
        result = {D::Conflict, "cannot restore execution or its deadline", {}};
    }
    p.decision = result;
    spdlog::warn("[recovery] robot={} mission={} result=Conflict reason={}",
                 id, p.head->mission().mission_id(), result.reason);
    return false;
}

bool FleetRegistry::remove(const std::string& robot_id) {
    auto it = sessions_.find(robot_id);
    if (it == sessions_.end()) {
        return false;
    }
    it->second->abort_mission("robot " + robot_id + " was removed");   // 6
    it->second->stop();

    queue_.remove(robot_id);
    probes_.erase(robot_id);

    sessions_.erase(it);

    if (inventory_) inventory_->deleting.erase(robot_id);

    status_hub_.remove(robot_id);
    spdlog::info("[registry] robot {} removed, total {}", robot_id, sessions_.size());
    return true;
}

void FleetRegistry::stop_all() {
    stopping_ = true;
    probe_timer_.cancel();
    for (auto& [id, session] : sessions_) {
        session->stop();
    }
    sessions_.clear();
}

RobotSession* FleetRegistry::find(const std::string& robot_id) {
    auto it = sessions_.find(robot_id);
    return it == sessions_.end() ? nullptr : it->second.get();
}

RobotSession* FleetRegistry::find_by_mission(const std::string& mission_id) {
    for (auto& [id, session] : sessions_) {
        if (session->mission() && session->mission()->mission_id() == mission_id) {
            return session.get();
        }
    }
    return nullptr;
}

bool FleetRegistry::post(const std::string& manufacturer, const std::string& serial_number, VdaMessage message) {
    RobotSession* session = find_by_vda(manufacturer, serial_number);
    if (!session) {
        return false;
    }
    return session->post(std::move(message));
}

RobotSession* FleetRegistry::find_by_vda(const std::string& manufacturer, const std::string& serial_number) {
    for (auto& [id, session] : sessions_) {
        if (session->manufacturer() == manufacturer && session->serial_number() == serial_number) {
            return session.get();
        }
    }
    return nullptr;
}