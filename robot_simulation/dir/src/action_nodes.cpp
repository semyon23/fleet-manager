#include "action_nodes.hpp"

#include <iostream>

using BT::NodeStatus;

// ----------------------------------------------------------------------------
// RobotAccess
// ----------------------------------------------------------------------------

RobotBehaviorApi& RobotAccess::robot(const BT::TreeNode& node) {
    if (!robot_) {
        // "@" — корневой blackboard: у каждого SubTree свой blackboard,
        // и без префикса ноды внутри поддеревьев робота не увидят.
        if (!node.config().blackboard->get("@robot_instance", robot_) || !robot_) {
            throw BT::RuntimeError("[BT] ", node.name(),
                                   ": 'robot_instance' (std::shared_ptr<RobotBehaviorApi>) "
                                   "is not on the blackboard");
        }
    }
    return *robot_;
}

// ----------------------------------------------------------------------------
// Условия
// ----------------------------------------------------------------------------

RobotCondition::RobotCondition(const std::string& name, const BT::NodeConfig& config,
                               Predicate predicate)
    : BT::ConditionNode(name, config), predicate_(predicate) {}

NodeStatus RobotCondition::tick() {
    return (robot(*this).*predicate_)() ? NodeStatus::SUCCESS : NodeStatus::FAILURE;
}

// ----------------------------------------------------------------------------
// WaitForOrder
// ----------------------------------------------------------------------------

NodeStatus WaitForOrder::onStart() {
    auto& r = robot(*this);
    // Order мог прийти раньше, чем дерево дошло до этой ноды — не ждём лишний тик.
    if (r.has_active_order()) {
        return NodeStatus::SUCCESS;
    }
    std::cout << "[BT] " << r.name() << ": waiting for order\n";
    return NodeStatus::RUNNING;
}

NodeStatus WaitForOrder::onRunning() {
    auto& r = robot(*this);
    if (r.has_active_order()) {
        std::cout << "[BT] " << r.name() << ": order received\n";
        return NodeStatus::SUCCESS;
    }
    return NodeStatus::RUNNING;
}

// ----------------------------------------------------------------------------
// HoldPosition
// ----------------------------------------------------------------------------

NodeStatus HoldPosition::onStart() {
    auto& r = robot(*this);
    r.stop_driving();
    std::cout << "[BT] " << r.name() << ": paused, holding position\n";
    return NodeStatus::RUNNING;
}

NodeStatus HoldPosition::onRunning() {
    return NodeStatus::RUNNING;  // завершает только родитель, когда пауза снята
}

void HoldPosition::onHalted() {
    std::cout << "[BT] " << robot(*this).name() << ": pause released\n";
}

// ----------------------------------------------------------------------------
// DriveSegment
// ----------------------------------------------------------------------------

NodeStatus DriveSegment::onStart() {
    auto& r = robot(*this);
    if (!r.start_segment()) {
        return NodeStatus::SUCCESS;  // ехать некуда — стоим там, где нужно
    }
    std::cout << "[BT] " << r.name() << ": driving\n";
    return NodeStatus::RUNNING;
}

NodeStatus DriveSegment::onRunning() {
    auto& r = robot(*this);

    // Update продлил base во время езды — дописать путь, не останавливаясь.
    if (r.extend_segment()) {
        std::cout << "[BT] " << r.name() << ": base extended while driving\n";
    }

    switch (r.drive_status()) {
        case RobotBehaviorApi::DriveStatus::Arrived:
            std::cout << "[BT] " << r.name() << ": segment done\n";
            return NodeStatus::SUCCESS;
        case RobotBehaviorApi::DriveStatus::Failed:
            // Робот сам сообщает NODE_UNREACHABLE и ждёт решения fleet manager (6.4.5).
            std::cout << "[BT] " << r.name() << ": cannot reach node\n";
            r.stop_driving();
            return NodeStatus::FAILURE;
        case RobotBehaviorApi::DriveStatus::Driving:
            break;
    }
    return NodeStatus::RUNNING;
}

void DriveSegment::onHalted() {
    // Пауза, отмена, авария, order выполнен — всегда безопасно, даже если робот уже стоит.
    robot(*this).stop_driving();
}

// ----------------------------------------------------------------------------
// ExecuteNodeActions
// ----------------------------------------------------------------------------

NodeStatus ExecuteNodeActions::onStart() {
    auto& r = robot(*this);
    r.start_node_actions();
    if (r.node_actions_done()) {
        return NodeStatus::SUCCESS;  // action'ов нет или они мгновенные
    }
    std::cout << "[BT] " << r.name() << ": executing node actions\n";
    return NodeStatus::RUNNING;
}

NodeStatus ExecuteNodeActions::onRunning() {
    // FAILED action — не ошибка ноды: статус уже в state, order продолжается.
    return robot(*this).node_actions_done() ? NodeStatus::SUCCESS : NodeStatus::RUNNING;
}

void ExecuteNodeActions::onHalted() {
    // Прерывание ноды (пауза, авария, смена режима) — не отмена order'а:
    // action'ы приостанавливает (startPause) или отменяет (cancelOrder) сам робот.
}

// ----------------------------------------------------------------------------
// WaitForBaseExtension
// ----------------------------------------------------------------------------

NodeStatus WaitForBaseExtension::onStart() {
    auto& r = robot(*this);
    if (r.has_base_ahead()) {
        return NodeStatus::SUCCESS;  // update уже пришёл
    }
    r.set_new_base_request(true);
    std::cout << "[BT] " << r.name() << ": at decision point, waiting for order update\n";
    return NodeStatus::RUNNING;
}

NodeStatus WaitForBaseExtension::onRunning() {
    auto& r = robot(*this);
    if (r.has_base_ahead()) {
        r.set_new_base_request(false);
        std::cout << "[BT] " << r.name() << ": base extended\n";
        return NodeStatus::SUCCESS;
    }
    return NodeStatus::RUNNING;
}

void WaitForBaseExtension::onHalted() {
    robot(*this).set_new_base_request(false);
}

// ----------------------------------------------------------------------------
// Регистрация
// ----------------------------------------------------------------------------

void register_action_nodes(BT::BehaviorTreeFactory& factory) {
    using Api = RobotBehaviorApi;
    factory.registerNodeType<RobotCondition>("IsSafe", &Api::is_safe);
    factory.registerNodeType<RobotCondition>("IsAutomaticMode", &Api::is_automatic_mode);
    factory.registerNodeType<RobotCondition>("IsPaused", &Api::is_paused);
    factory.registerNodeType<RobotCondition>("HasActiveOrder", &Api::has_active_order);
    factory.registerNodeType<RobotCondition>("HasBaseAhead", &Api::has_base_ahead);
    factory.registerNodeType<RobotCondition>("AtDecisionPoint", &Api::at_decision_point);

    factory.registerNodeType<WaitForOrder>("WaitForOrder");
    factory.registerNodeType<HoldPosition>("HoldPosition");
    factory.registerNodeType<DriveSegment>("DriveSegment");
    factory.registerNodeType<ExecuteNodeActions>("ExecuteNodeActions");
    factory.registerNodeType<WaitForBaseExtension>("WaitForBaseExtension");
}
