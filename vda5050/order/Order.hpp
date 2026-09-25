#ifndef FLEETMANAGERSYSTEM_ORDER_HPP
#define FLEETMANAGERSYSTEM_ORDER_HPP

    #pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>



// Константы-маркеры для неустановленных значений в приказах
namespace order_sentinel {
    constexpr double NO_VALUE = -1.0;
    constexpr double NO_ANGLE = -999.0;
}

// Параметры для конкретного действия (Action), например: {"key": "height", "value": "1.2"}
struct ActionParameter {
    std::string key;
    std::string value;
};

// Описание действия, которое робот должен выполнить в точке или на ребре
struct Action {
    std::string actionId;           // Уникальный ID действия (генерируется Диспетчером)
    std::string actionType;         // Тип ("liftPalette", "dropPalette", "beep", "wait")
    std::string blockingType;       // Влияние на движение: "NONE", "SOFT", "HARD"
    std::vector<ActionParameter> actionParameters; // Специфичные параметры действия
};

// Описание геометрии точки (Node), куда должен приехать робот
struct NodePosition {
    double x = 0.0;
    double y = 0.0;
    double theta = order_sentinel::NO_ANGLE; // Угол поворота на точке. -999.0 = не важен
    std::string mapId;                       // ID карты помещения
    double allowedDeviationXY = order_sentinel::NO_VALUE;  // Допустимый радиус ошибки (м)
    double allowedDeviationTheta = order_sentinel::NO_VALUE; // Допустимая ошибка угла (рад)
};

struct Node {
    std::string nodeId;
    uint32_t sequenceId = 0;
    std::string nodeDescription;
    bool released = true;
    NodePosition nodePosition;
    std::vector<Action> actions;
};

struct Edge {
    std::string edgeId;
    uint32_t sequenceId = 0;
    std::string edgeDescription;
    bool released = true;           // Разрешено ли движение по ребру прямо сейчас
    std::string trajectoryType;     // Тип траектории: "LINE", "SPLINE", "BEZIER" (v3.0)
    std::vector<Action> actions;    // Действия, выполняемые ВО ВРЕМЯ движения по ребру
};

struct OrderMessage {
    uint32_t headerId = 0;          // Инкрементальный счетчик сообщений от Диспетчера к роботу
    std::string timestamp;          // Время отправки ISO 8601 UTC
    std::string version = "3.0.0";  // Версия протокола

    std::string orderId;            // Глобальный ID миссии (например, "task_move_palette_987")
    uint32_t orderUpdateId = 0;     // Инкремент при допихивании новых точек в текущую миссию

    std::vector<Node> nodes;        // Массив точек маршрута (базовый граф путей)
    std::vector<Edge> edges;        // Массив ребер между точками
};

// === МАКРОСЫ СЕРИАЛИЗАЦИИ JSON ===
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ActionParameter, key, value)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Action, actionId, actionType, blockingType, actionParameters)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NodePosition, x, y, theta, mapId, allowedDeviationXY, allowedDeviationTheta)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Node, nodeId, sequenceId, nodeDescription, released, nodePosition, actions)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Edge, edgeId, sequenceId, edgeDescription, released, trajectoryType, actions)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OrderMessage,
    headerId, timestamp, version,
    orderId, orderUpdateId, nodes, edges
)



#endif //FLEETMANAGERSYSTEM_ORDER_HPP
