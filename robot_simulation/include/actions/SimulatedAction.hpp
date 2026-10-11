
#ifndef SIMULATEDACTION_HPP
#define SIMULATEDACTION_HPP

#include "ActionExecutor.hpp"

namespace sim_action {
    inline constexpr const char* kType = "simulatedAction";
}

void register_simulated_action(ActionExecutor& executor, double default_duration = 3.0);


#endif //SIMULATEDACTION_HPP
