#ifndef FLEETMANAGERSYSTEM_PROBEQUEUE_HPP
#define FLEETMANAGERSYSTEM_PROBEQUEUE_HPP

#pragma once

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

class ProbeQueue {
public:
    ProbeQueue() = default;
    ProbeQueue(const ProbeQueue&) = delete;
    ProbeQueue& operator=(const ProbeQueue&) = delete;

    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    static constexpr auto timeout = std::chrono::seconds(5);
    static constexpr auto delay = std::chrono::seconds(2);
    static constexpr std::size_t limit = 16;

    void add(const std::string& id, Time when) {
        if (waiting_.contains(id) || active_.contains(id)) return;
        waiting_.emplace(id, due_.emplace(when, id));
    }

    void remove(const std::string& id) {
        if (auto it = waiting_.find(id); it != waiting_.end()) {
            due_.erase(it->second);
            waiting_.erase(it);
        }
        active_.erase(id);
    }

    void retry(const std::string& id, Time now) {
        remove(id);
        add(id, now + delay);
    }

    bool live(const std::string& id, Time now) const {
        auto it = active_.find(id);
        return it != active_.end() && now < it->second;
    }

    bool empty() const {
        return active_.empty() && due_.empty();
    }

    std::vector<std::string> take(Time now) {
        for (auto it = active_.begin(); it != active_.end();) {
            if (now < it->second) {
                ++it;
                continue;
            }
            auto id = it->first;
            it = active_.erase(it);
            add(id, now + delay);
        }

        std::vector<std::string> batch;
        while (active_.size() < limit && !due_.empty() && due_.begin()->first <= now) {
            auto id = due_.begin()->second;
            remove(id);
            active_.emplace(id, now + timeout);
            batch.push_back(std::move(id));
        }
        return batch;
    }

private:
    std::multimap<Time, std::string> due_;
    std::map<std::string, decltype(due_)::iterator> waiting_;
    std::map<std::string, Time> active_;
};


#endif //FLEETMANAGERSYSTEM_PROBEQUEUE_HPP
