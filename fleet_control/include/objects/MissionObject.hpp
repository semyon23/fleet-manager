
#ifndef MISSIONOBJECT_HPP
#define MISSIONOBJECT_HPP

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace fms {

enum class MissionStatus {
    PENDING,    // Ждет в очереди (свободных роботов нужного класса пока нет)
    ASSIGNED,   // Робот выбран, команда формируется для отправки
    EXECUTING,  // Робот принял задачу и физически едет по цеху
    COMPLETED,  // Робот доложил об успешном завершении всех точек маршрута
    FAILED,     // Критическая ошибка (робот застрял, ушел в аварийный стоп)
    CANCELLED   // Миссия была принудительно отменена оператором из UI
};

inline std::string to_string(MissionStatus status) {
    switch (status) {
        case MissionStatus::PENDING:   return "PENDING";
        case MissionStatus::ASSIGNED:  return "ASSIGNED";
        case MissionStatus::EXECUTING: return "EXECUTING";
        case MissionStatus::COMPLETED: return "COMPLETED";
        case MissionStatus::FAILED:    return "FAILED";
        case MissionStatus::CANCELLED: return "CANCELLED";
    }
    return "PENDING";
}

struct RouteNode {
    std::string node_id;
    uint32_t sequence_id = 0;
    bool released = true;
};

struct RouteEdge {
    std::string edge_id;
    uint32_t sequence_id = 0;
    bool released = true;
};

class MissionObject {
public:
    std::string id;                  // Уникальный ID задачи (UUID / GUID)
    std::string required_agv_class;  // Какой класс робота требуется (FORKLIFT, CARRIER и т.д.)
    std::string assigned_robot;      // Имя робота, который выполняет эту задачу (пусто, если PENDING)
    MissionStatus status = MissionStatus::PENDING;

    std::vector<RouteNode> nodes;
    std::vector<RouteEdge> edges;

    json raw_order = json::object();

    MissionObject() = default;

    MissionObject(std::string mission_id, std::string agv_class)
        : id(std::move(mission_id)), required_agv_class(std::move(agv_class)) {}
};

} // namespace fms


#endif //MISSIONOBJECT_HPP
