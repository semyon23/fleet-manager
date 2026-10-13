#ifndef FLEETMANAGERSYSTEM_ACTIONMANAGER_HPP
#define FLEETMANAGERSYSTEM_ACTIONMANAGER_HPP

#include <string>
#include <vector>

#include "vda5050.hpp"
#include "include/actions/ActionExecutor.hpp"
#include "OrderManager.hpp"
#include "include/motion/RobotDriveController.hpp"
#include "StatePublisher.hpp"

class ActionManager {
public:
    ActionManager(vda5050::State& state, ActionExecutor& executor, RobotDriveController& driver,
                  OrderManager& order_manager, StatePublisher& publisher);

    void handle_instant_actions(const std::string& json);
    // Каждый тик после executor.update(): cancelOrder -> FINISHED, когда доработали неотменяемые.
    void update_pending_cancel();

private:
    void execute_instant_action(const vda5050::Action& a);
    void cancel_order(const vda5050::Action& a);
    void set_paused(const vda5050::Action& a, bool paused);
    void initialize_position(const vda5050::Action& a);
    void set_instant_status(const std::string& action_id, vda5050::ActionStatus status);
    void add_warning(const std::string& type, const std::string& description,
                     std::vector<vda5050::ErrorReference> refs);

    vda5050::State& State_;
    ActionExecutor& action_executor_;
    RobotDriveController& driver_;
    OrderManager& order_manager_;
    StatePublisher& publisher_;

    std::vector<std::string> pending_cancel_ids_;
};


#endif //FLEETMANAGERSYSTEM_ACTIONMANAGER_HPP
