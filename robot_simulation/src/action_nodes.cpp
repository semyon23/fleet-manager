#include <include/action_nodes.hpp>
#include <print>
RobotBehaviorApi& RobotAccess::robot(const BT::TreeNode& node) {
    if (!robot_) {
        if (!node.config().blackboard->get("@robot_api", robot_) || !robot_) {
            throw BT::RuntimeError("[BT] ", node.name(),
                                   ": 'robot_api' (std::shared_ptr<RobotBehaviorApi>) "
                                   "is not on the blackboard");
        }
    }
    return *robot_;
}

RobotCondition::RobotCondition(const std::string& name, const BT::NodeConfig& config,
                               ConditionMethod method)
    : BT::ConditionNode(name, config), method_(method) {}

BT::NodeStatus RobotCondition::tick() {
    return (robot(*this).*method_)() ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
}

// ----------------------------------------------------------------------------
// WaitForOrder
// ----------------------------------------------------------------------------

BT::NodeStatus WaitForOrder::onStart() {
    auto& robot_api = robot(*this);
    if (robot_api.has_active_order()) {
        return BT::NodeStatus::SUCCESS;
    }
    std::println("[BT][{}] waiting for order", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus WaitForOrder::onRunning() {
    auto& robot_api = robot(*this);
    if (robot_api.has_active_order()) {
        std::println("[BT][{}] order received", robot_api.name());
        return BT::NodeStatus::SUCCESS;
    }
    return BT::NodeStatus::RUNNING;
}

void WaitForOrder::onHalted(){
    auto& robot_api = robot(*this);
    std::println("[BT][{}] Wait for Order on halted", robot_api.name());
}

// ----------------------------------------------------------------------------
// HoldPosition
// ----------------------------------------------------------------------------

BT::NodeStatus HoldPosition::onStart() {
    auto& robot_api = robot(*this);
    robot_api.stop_driving();
    std::println("[BT][{}]  paused, holding position", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus HoldPosition::onRunning() {
    return BT::NodeStatus::RUNNING;
}

void HoldPosition::onHalted() {
    auto& robot_api = robot(*this);
    std::println("[BT][{}]   pause released", robot_api.name());
}

// ----------------------------------------------------------------------------
// DriveSegment
// ----------------------------------------------------------------------------

BT::NodeStatus DriveSegment::onStart() {
    auto& robot_api = robot(*this);
    if (!robot_api.start_segment()) {
        return BT::NodeStatus::SUCCESS;
    }
    std::println("[BT][{}] driving", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus DriveSegment::onRunning() {
    auto& robot_api = robot(*this);

    // Update продлил base во время езды — дописать путь, не останавливаясь.
    if (robot_api.extend_segment()) {
        std::cout << "[BT] " << robot_api.name() << ": base extended while driving\n";
    }

    switch (robot_api.drive_status()) {
    case RobotBehaviorApi::DriveStatus::Arrived:
        std::println("[BT][{}] segment done", robot_api.name());
        return BT::NodeStatus::SUCCESS;
    case RobotBehaviorApi::DriveStatus::Failed:
        std::println("[BT][{}] cannot reach node", robot_api.name());
        robot_api.stop_driving();
        return BT::NodeStatus::FAILURE;
    case RobotBehaviorApi::DriveStatus::Driving:
        break;
    }
    return BT::NodeStatus::RUNNING;
}

void DriveSegment::onHalted() {
    robot(*this).stop_driving();
    std::println("[BT][{}] driving on halted", robot(*this).name());
}

// ----------------------------------------------------------------------------
// ExecuteNodeActions
// ----------------------------------------------------------------------------

BT::NodeStatus ExecuteNodeActions::onStart() {
    auto& robot_api = robot(*this);
    robot_api.start_node_actions();
    if (robot_api.node_actions_done()) {
        std::println("[BT][{}] executing node actions done", robot_api.name());
        return BT::NodeStatus::SUCCESS;
    }
    std::println("[BT][{}] executing node actions start", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus ExecuteNodeActions::onRunning() {

    return robot(*this).node_actions_done() ? BT::NodeStatus::SUCCESS : BT::NodeStatus::RUNNING;
}

void ExecuteNodeActions::onHalted() {
    robot(*this).cancel_node_actions();
}

// ----------------------------------------------------------------------------
// WaitForBaseExtension
// ----------------------------------------------------------------------------

BT::NodeStatus WaitForBaseExtension::onStart() {
    auto& robot_api = robot(*this);
    if (robot_api.has_base_ahead()) {
        return BT::NodeStatus::SUCCESS;
    }
    robot_api.set_new_base_request(true);
    std::println("[BT][{}] at decision point, waiting for order update", robot_api.name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus WaitForBaseExtension::onRunning() {
    auto& robot_api = robot(*this);
    if (robot_api.has_base_ahead()) {
        robot_api.set_new_base_request(false);
        std::println("[BT][{}] base extended", robot_api.name());
        return BT::NodeStatus::SUCCESS;
    }
    return BT::NodeStatus::RUNNING;
}

void WaitForBaseExtension::onHalted() {
    robot(*this).set_new_base_request(false);
}

void register_action_nodes(BT::BehaviorTreeFactory& factory) {
    factory.registerBuilder<RobotCondition>("IsSafe",
        [](const std::string& name, const BT::NodeConfig& config) {
            return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::is_safe);
        });

    factory.registerBuilder<RobotCondition>("IsAutomaticMode",
        [](const std::string& name, const BT::NodeConfig& config) {
            return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::is_automatic_mode);
        });

    factory.registerBuilder<RobotCondition>("IsPaused",
        [](const std::string& name, const BT::NodeConfig& config) {
            return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::is_paused);
        });

    factory.registerBuilder<RobotCondition>("HasActiveOrder",
        [](const std::string& name, const BT::NodeConfig& config) {
            return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::has_active_order);
        });

    factory.registerBuilder<RobotCondition>("HasBaseAhead",
        [](const std::string& name, const BT::NodeConfig& config) {
            return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::has_base_ahead);
        });

    factory.registerBuilder<RobotCondition>("AtDecisionPoint",
        [](const std::string& name, const BT::NodeConfig& config) {
            return std::make_unique<RobotCondition>(name, config, &RobotBehaviorApi::at_decision_point);
        });

    // Для остальных асинхронных узлов со стандартными конструкторами оставляем как было
    factory.registerNodeType<WaitForOrder>("WaitForOrder");
    factory.registerNodeType<HoldPosition>("HoldPosition");
    factory.registerNodeType<DriveSegment>("DriveSegment");
    factory.registerNodeType<ExecuteNodeActions>("ExecuteNodeActions");
    factory.registerNodeType<WaitForBaseExtension>("WaitForBaseExtension");

}











// factory.registerNodeType<RobotCondition>("IsSafe", &RobotBehaviorApi::is_safe);
// factory.registerNodeType<RobotCondition>("IsAutomaticMode", &RobotBehaviorApi::is_automatic_mode);
// factory.registerNodeType<RobotCondition>("IsPaused", &RobotBehaviorApi::is_paused);
// factory.registerNodeType<RobotCondition>("HasActiveOrder", &RobotBehaviorApi::has_active_order);
// factory.registerNodeType<RobotCondition>("HasBaseAhead", &RobotBehaviorApi::has_base_ahead);
// factory.registerNodeType<RobotCondition>("AtDecisionPoint", &RobotBehaviorApi::at_decision_point);
//
// factory.registerNodeType<WaitForOrder>("WaitForOrder");
// factory.registerNodeType<HoldPosition>("HoldPosition");
// factory.registerNodeType<DriveSegment>("DriveSegment");
// factory.registerNodeType<ExecuteNodeActions>("ExecuteNodeActions");
// factory.registerNodeType<WaitForBaseExtension>("WaitForBaseExtension");








