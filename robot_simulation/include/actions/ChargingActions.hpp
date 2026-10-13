
#ifndef CHARGINGACTIONS_HPP
#define CHARGINGACTIONS_HPP

// startCharging / stopCharging (VDA 5050): instant и node, blockingType HARD.
// startCharging: RUNNING на время "стыковки", затем powerSupply.charging = true и FINISHED.
//   Зарядка продолжается и после FINISHED — до stopCharging или начала движения.
//   Робот едет — FAILED.
// stopCharging: charging = false, сразу FINISHED.

#include "ActionExecutor.hpp"
#include "../robot/robot_config.hpp"

void register_charging_actions(ActionExecutor& executor, const BatteryConfig& battery);


#endif //CHARGINGACTIONS_HPP
