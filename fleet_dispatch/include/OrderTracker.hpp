
#ifndef FLEETMANAGERSYSTEM_ORDERTRACKER_HPP
#define FLEETMANAGERSYSTEM_ORDERTRACKER_HPP
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include "vda5050.hpp"

class OrderTracker {
public:
    using Clock = std::chrono::steady_clock;

    enum class Phase {
        Sent,       // отправлен, робот ещё не подтвердил
        Accepted,   // робот выполняет
        Rejected,   // робот отказал или не ответил вовремя
        Finished,   // маршрут пройден, все действия закончены
        Failed,     // действие FAILED или робот выполняет уже не наш order
    };

    OrderTracker(const vda5050::Order& order, Clock::time_point sent_at);

    void on_state(const vda5050::State& state);   // каждый state робота
    void on_tick(Clock::time_point now);          // таймаут «не принят»

    Phase phase() const { return phase_; }
    bool done() const { return phase_ == Phase::Rejected || phase_ == Phase::Finished || phase_ == Phase::Failed; }
    const std::string& order_id() const { return order_id_; }
    int passed_nodes() const { return passed_nodes_; }        // сколько узлов маршрута робот уже достиг
    int total_nodes() const { return total_nodes_; }
    bool waiting_for_release() const { return waiting_for_release_; }   // base кончилась, ждёт продления
    const std::string& failure() const { return failure_; }   // причина при Rejected / Failed

private:
    void finish(Phase phase, std::string failure = {});
    std::string order_error(const vda5050::State& state) const;

    std::string order_id_;
    std::uint32_t order_update_id_;
    Clock::time_point sent_at_;
    int total_nodes_;
    std::unordered_map<std::string, vda5050::ActionStatus> actions_;

    Phase phase_ = Phase::Sent;
    int passed_nodes_ = 0;
    bool waiting_for_release_ = false;
    std::string failure_;
};

const char* to_string(OrderTracker::Phase phase);   // для логов


#endif //FLEETMANAGERSYSTEM_ORDERTRACKER_HPP
