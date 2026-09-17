

#ifndef TASKPOOL_HPP
#define TASKPOOL_HPP



#pragma once
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <string>
#include <optional>
//#include "models.hpp" // Тут лежат наши структуры MissionObject
class MissionObject;
class TaskPool {
public:
    explicit TaskPool(size_t max_capacity = 5000) : max_capacity_(max_capacity) {}

    // Запрещаем копирование пула для безопасности потоков
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    // Публичные потокобезопасные методы
    bool push_task(const MissionObject& mission);
    std::vector<MissionObject> get_all_pending_tasks() const;
    void remove_task(const std::string& mission_id);
    std::optional<MissionObject> find_task(const std::string& mission_id) const;

private:
    mutable std::shared_mutex rw_mutex_;
    std::unordered_map<std::string, MissionObject> tasks_;
    size_t max_capacity_;
};



#endif //TASKPOOL_HPP
 