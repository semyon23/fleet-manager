
#ifndef FACTSHEET_BUILDER_HPP
#define FACTSHEET_BUILDER_HPP

#include <set>
#include <string>

#include "include/robot/robot_config.hpp"
#include "vda5050.hpp"

// supported_actions — action'ы, для которых у робота есть обработчик
// (ActionExecutor::supported_types()). Обязательные cancelOrder/startPause/stopPause
// и служебные factsheetRequest/stateRequest добавляются всегда.
vda5050::Factsheet build_factsheet(const RobotConfig& config,
                                   const std::set<std::string>& supported_actions);
#endif //FACTSHEET_BUILDER_HPP
