#ifndef FLEETMANAGERSYSTEM_ACTIONMANAGER_HPP
#define FLEETMANAGERSYSTEM_ACTIONMANAGER_HPP

#include "../vda5050/vda5050.hpp"

class ActionManager{
public:
    ActionManager(vda5050::State& State);
    void handle_instant_actions(const std::string& json);
private:
    void execute_instant_action(const vda5050::Action& a);
    void set_paused(const vda5050::Action& a, bool paused);
    void cancel_order(const vda5050::Action& a);
    void set_instant_status(const std::string& action_id, vda5050::ActionStatus status);
    void initialize_position(const vda5050::Action& a);
    void update_pending_cancel();
    void add_warning(const std::string& type, const std::string& description, std::vector<vda5050::ErrorReference> refs);
    vda5050::State& State_;

};


#endif //FLEETMANAGERSYSTEM_ACTIONMANAGER_HPP
