
#ifndef FLEETMANAGERSYSTEM_ROBOTBEHAVIORIMPL_HPP
#define FLEETMANAGERSYSTEM_ROBOTBEHAVIORIMPL_HPP
#include "include/action_nodes.hpp"
#include "include/RobotSimulator.hpp"

class RobotBehaviorImpl : public  RobotBehaviorApi{
public:
    RobotBehaviorImpl(std::shared_ptr<RobotSimulator> robot);
    std::string name() const override;
    bool is_safe() const override;
    bool is_automatic_mode() const override;
    bool is_paused() const override;
    bool has_active_order() const override;
    bool has_base_ahead() const override;
    bool at_decision_point() const override;

    bool start_segment() override;
    bool extend_segment() override;
    DriveStatus drive_status() const override;
    void stop_driving() override;

    void start_node_actions() override;
    bool node_actions_done() const override;
    void cancel_node_actions() override;
    void set_new_base_request(bool value) override;
private:
    std::shared_ptr<RobotSimulator> robot_ptr;
};


#endif //FLEETMANAGERSYSTEM_ROBOTBEHAVIORIMPL_HPP
