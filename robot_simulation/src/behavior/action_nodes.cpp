#include <include/behavior/action_nodes.hpp>
#include <spdlog/spdlog.h>

RobotBehaviorApi& RobotAccess::robot(const BT::TreeNode& node){
    if (!robot_){
        if (!node.config().blackboard->get("@robot_api", robot_) || !robot_){
            throw BT::RuntimeError("[BT] ", node.name(),
                                   ": 'robot_api' (RobotBehaviorApi*) is not on the blackboard");
        }
    }
    return *robot_;
}

RobotCondition::RobotCondition(const std::string& name, const BT::NodeConfig& config, ConditionMethod method)
    : BT::ConditionNode(name, config), method_(method){
}

BT::NodeStatus RobotCondition::tick(){
    return (robot(*this).*method_)() ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
}

// ----------------------------------------------------------------------------
// WaitForOrder
// ----------------------------------------------------------------------------

BT::NodeStatus WaitForOrder::onStart(){
    auto& robot_api = robot(*this);
    if (robot_api.has_active_order()){
        return BT::NodeStatus::SUCCESS;
    }
    spdlog::info("[behavior_tree][{}] Waiting for order", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus WaitForOrder::onRunning(){
    auto& robot_api = robot(*this);
    if (robot_api.has_active_order()){
        spdlog::info("[behavior_tree][{}] Order received", robot_api.name());
        return BT::NodeStatus::SUCCESS;
    }
    return BT::NodeStatus::RUNNING;
}

void WaitForOrder::onHalted(){
    auto& robot_api = robot(*this);
    spdlog::info("[behavior_tree][{}]  Wait for Order on halted", robot_api.name());
}

// ----------------------------------------------------------------------------
// HoldPosition
// ----------------------------------------------------------------------------

BT::NodeStatus HoldPosition::onStart(){
    auto& robot_api = robot(*this);
    robot_api.stop_driving();
    spdlog::info("[behavior_tree][{}] Paused, holding position", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus HoldPosition::onRunning(){
    return BT::NodeStatus::RUNNING;
}

void HoldPosition::onHalted(){
    auto& robot_api = robot(*this);
    spdlog::info("[behavior_tree][{}] Pause released", robot_api.name());
}

// ----------------------------------------------------------------------------
// DriveSegment
// ----------------------------------------------------------------------------

BT::NodeStatus DriveSegment::onStart(){
    auto& robot_api = robot(*this);
    if (!robot_api.start_segment()){
        return BT::NodeStatus::SUCCESS;
    }
    spdlog::info("[behavior_tree][{}] Driving", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus DriveSegment::onRunning(){
    auto& robot_api = robot(*this);

    if (robot_api.extend_segment()){
        spdlog::info("[behavior_tree][{}] base extended while driving", robot_api.name());
    }

    switch (robot_api.drive_status()){
    case RobotBehaviorApi::DriveStatus::Arrived:
        spdlog::info("[behavior_tree][{}] segment done", robot_api.name());
        return BT::NodeStatus::SUCCESS;
    case RobotBehaviorApi::DriveStatus::Failed:
        spdlog::info("[behavior_tree][{}] cannot reach node", robot_api.name());
        robot_api.stop_driving();
        return BT::NodeStatus::FAILURE;
    case RobotBehaviorApi::DriveStatus::Driving:
        break;
    }
    return BT::NodeStatus::RUNNING;
}

void DriveSegment::onHalted(){
    robot(*this).stop_driving();
    spdlog::info("[behavior_tree][{}] driving on halted", robot(*this).name());
}

// ----------------------------------------------------------------------------
// ExecuteNodeActions
// ----------------------------------------------------------------------------

BT::NodeStatus ExecuteNodeActions::onStart(){
    auto& robot_api = robot(*this);
    robot_api.start_node_actions();
    if (robot_api.node_actions_done()){
        spdlog::info("[behavior_tree][{}] executing node actions done", robot_api.name());
        return BT::NodeStatus::SUCCESS;
    }
    spdlog::info("[behavior_tree][{}] executing node actions start", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus ExecuteNodeActions::onRunning(){
    return robot(*this).node_actions_done() ? BT::NodeStatus::SUCCESS : BT::NodeStatus::RUNNING;
}

void ExecuteNodeActions::onHalted(){
    spdlog::info("[behavior_tree][{}] execute actions on halted", robot(*this).name());
}

// ----------------------------------------------------------------------------
// WaitForBaseExtension
// ----------------------------------------------------------------------------

BT::NodeStatus WaitForBaseExtension::onStart(){
    auto& robot_api = robot(*this);
    if (robot_api.has_base_ahead()){
        return BT::NodeStatus::SUCCESS;
    }
    robot_api.set_new_base_request(true);
    spdlog::info("[behavior_tree][{}] at decision point, waiting for order update", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus WaitForBaseExtension::onRunning(){
    auto& robot_api = robot(*this);
    if (robot_api.has_base_ahead()){
        robot_api.set_new_base_request(false);
        spdlog::info("[behavior_tree][{}] base extended", robot_api.name());
        return BT::NodeStatus::SUCCESS;
    }
    return BT::NodeStatus::RUNNING;
}

void WaitForBaseExtension::onHalted(){
    robot(*this).set_new_base_request(false);
}

void register_action_nodes(BT::BehaviorTreeFactory& factory){
    factory.registerBuilder<RobotCondition>("IsSafe", [](const std::string& name, const BT::NodeConfig& config){
        return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::is_safe);
    });

    factory.registerBuilder<RobotCondition>("IsAutomaticMode",
                                            [](const std::string& name, const BT::NodeConfig& config){
                                                return std::make_unique<RobotCondition>(
                                                    name, config, &RobotBehaviorApi::is_automatic_mode);
                                            });

    factory.registerBuilder<RobotCondition>("IsPaused", [](const std::string& name, const BT::NodeConfig& config){
        return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::is_paused);
    });

    factory.registerBuilder<RobotCondition>("HasActiveOrder", [](const std::string& name, const BT::NodeConfig& config){
        return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::has_active_order);
    });

    factory.registerBuilder<RobotCondition>("HasBaseAhead", [](const std::string& name, const BT::NodeConfig& config){
        return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::has_base_ahead);
    });

    factory.registerBuilder<RobotCondition>("AtDecisionPoint",
                                            [](const std::string& name, const BT::NodeConfig& config){
                                                return std::make_unique<RobotCondition>(
                                                    name, config, &RobotBehaviorApi::at_decision_point);
                                            });

    factory.registerNodeType<WaitForOrder>("WaitForOrder");
    factory.registerNodeType<HoldPosition>("HoldPosition");
    factory.registerNodeType<DriveSegment>("DriveSegment");
    factory.registerNodeType<ExecuteNodeActions>("ExecuteNodeActions");
    factory.registerNodeType<WaitForBaseExtension>("WaitForBaseExtension");
}
