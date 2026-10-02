#include "../include/RobotBehaviorImpl.hpp"

RobotBehaviorImpl::RobotBehaviorImpl(std::shared_ptr<RobotSimulator> robot)
    : robot_ptr(robot){};


std::string RobotBehaviorImpl::name() const{
    return robot_ptr->get_name();
}

bool RobotBehaviorImpl::is_safe() const{
    auto state = robot_ptr->get_state();
    return state.safety_state.active_emergency_stop == vda5050::EmergencyStop::None && !state.safety_state.field_violation;
}

bool RobotBehaviorImpl::is_automatic_mode() const{
    auto state = robot_ptr->get_state();
    return state.operating_mode == vda5050::OperatingMode::Automatic ||
          state.operating_mode == vda5050::OperatingMode::Semiautomatic;
}

bool RobotBehaviorImpl::is_paused() const{
    auto state = robot_ptr->get_state();
    return state.paused.value_or(false);
}

bool RobotBehaviorImpl::has_active_order() const{
    auto state = robot_ptr->get_state();
    return !vda5050::is_idle(state);
}

bool RobotBehaviorImpl::has_base_ahead() const{
    auto order = robot_ptr->get_last_received_order();
    auto state = robot_ptr->get_state();
    for (const auto& node : order.nodes) {
        if (!node.released) {
            return false;
        }
        if (node.sequence_id > state.last_node_sequence_id) {
            return true;
        }
    }
    return false;
}

bool RobotBehaviorImpl::at_decision_point() const{
    auto state = robot_ptr->get_state();
    return !has_base_ahead() && !state.node_states.empty();
}

bool RobotBehaviorImpl::start_segment(){
    return robot_ptr->start_new_segment();
}

bool RobotBehaviorImpl::extend_segment(){
    return robot_ptr->extend_current_segment();
}

RobotBehaviorApi::DriveStatus RobotBehaviorImpl::drive_status() const{
    if (robot_ptr->path_finished() && !robot_ptr->is_moving()) {
        return DriveStatus::Arrived;
    }
    return DriveStatus::Driving;
}

void RobotBehaviorImpl::stop_driving(){
    robot_ptr->standstill();
}

void RobotBehaviorImpl::start_node_actions(){

}

void RobotBehaviorImpl::cancel_node_actions(){

}

bool RobotBehaviorImpl::node_actions_done() const{
    return true;
}

void RobotBehaviorImpl::set_new_base_request(bool value){
    robot_ptr->get_state().new_base_request = value;
}
