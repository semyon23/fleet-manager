#ifndef FLEETMANAGERSYSTEM_ORDERBUILDER_H
#define FLEETMANAGERSYSTEM_ORDERBUILDER_H

#pragma once

#include <string>

#include "fms_types.pb.h"
#include "vda5050.hpp"

// Проверить шаг до построения order'а. Пустая строка — всё в порядке, иначе текст ошибки
// (уйдёт в INVALID_ARGUMENT).
std::string validate_move_step(const fms::v1::MoveStep& step);
std::string validate_action_step(const fms::v1::ActionStep& step);

// MoveStep → VDA order: маршрут по узлам и рёбрам, base и horizon по released_nodes.
// Заголовок не заполняется — это делает сессия при отправке.
vda5050::Order build_move_order(const std::string& order_id, const fms::v1::MoveStep& step);

// ActionStep → VDA order из одного узла — того, где робот стоит, — с действиями на нём.
// У робота должны быть last_node_id и pose (он на графе).
vda5050::Order build_action_order(const std::string& order_id, const fms::v1::ActionStep& step,
                                  const fms::v1::RobotStatus& robot);

#endif //FLEETMANAGERSYSTEM_ORDERBUILDER_H
