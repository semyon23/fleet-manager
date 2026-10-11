#pragma once

// Action'ы pick и drop для симулятора (VDA 5050 v3.0.0, 6.2.3).
//
// Механизма нет, поэтому action просто отсчитывает время и по окончании
// меняет state.loads: pick добавляет груз, drop убирает.
// Параметры action'а (все необязательные): lhd, loadId, loadType.

#include "ActionExecutor.hpp"
#include "include/robot/robot_config.hpp"   // LoadHandlingConfig

// Регистрирует обработчики "pick" и "drop" в исполнителе.
void register_pick_drop(ActionExecutor& executor, const LoadHandlingConfig& config);
