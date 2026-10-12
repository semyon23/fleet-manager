#ifndef FLEETMANAGERSYSTEM_ACTION_NODES_HPP
#define FLEETMANAGERSYSTEM_ACTION_NODES_HPP
#pragma once

#include <memory>
#include <string>

#include "behaviortree_cpp/action_node.h"
#include "behaviortree_cpp/bt_factory.h"
#include "behaviortree_cpp/condition_node.h"


class RobotBehaviorApi {
public:
    enum class DriveStatus { Driving, Arrived, Failed };

    virtual ~RobotBehaviorApi() = default;

    virtual std::string name() const = 0;

    virtual bool is_safe() const = 0;            // нет аварийной остановки и нарушения поля
    virtual bool is_automatic_mode() const = 0;  // AUTOMATIC или SEMIAUTOMATIC
    virtual bool is_paused() const = 0;          // после startPause
    virtual bool has_active_order() const = 0;   // робот не idle (6.6.8)
    virtual bool has_base_ahead() const = 0;     // впереди есть released-узлы
    virtual bool at_decision_point() const = 0;  // released впереди нет, но horizon остался

    virtual bool start_segment() = 0;
    virtual bool extend_segment() = 0;
    virtual DriveStatus drive_status() const = 0;
    virtual void stop_driving() = 0;             // плавная остановка

    // --- action'ы узла, на котором стоит робот ---
    virtual void start_node_actions() = 0;       // запустить ещё не начатые
    virtual bool node_actions_done() const = 0;  // все FINISHED или FAILED


    virtual void set_new_base_request(bool value) = 0;
};

class RobotAccess {
protected:
    RobotBehaviorApi& robot(const BT::TreeNode& node);

private:
    RobotBehaviorApi* robot_ = nullptr;
};

class RobotCondition : public BT::ConditionNode, private RobotAccess {
public:
    using ConditionMethod  = bool (RobotBehaviorApi::*)() const;

    RobotCondition(const std::string& name, const BT::NodeConfig& config, ConditionMethod method);

    static BT::PortsList providedPorts() { return {}; }
    BT::NodeStatus tick() override;

private:
    ConditionMethod  method_;
};


class WaitForOrder : public BT::StatefulActionNode, private RobotAccess {
public:
    using BT::StatefulActionNode::StatefulActionNode;
    static BT::PortsList providedPorts() { return {}; }

    BT::NodeStatus onStart() override;
    BT::NodeStatus onRunning() override;
    void onHalted() override;
};

// Стоит на месте, пока родитель не прервёт (снятие паузы).
class HoldPosition : public BT::StatefulActionNode, private RobotAccess {
public:
    using BT::StatefulActionNode::StatefulActionNode;
    static BT::PortsList providedPorts() { return {}; }

    BT::NodeStatus onStart() override;
    BT::NodeStatus onRunning() override;
    void onHalted() override;
};

// Доехать до ближайшей точки остановки. Продлевает путь, если во время езды пришёл update.
class DriveSegment : public BT::StatefulActionNode, private RobotAccess {
public:
    using BT::StatefulActionNode::StatefulActionNode;
    static BT::PortsList providedPorts() { return {}; }

    BT::NodeStatus onStart() override;
    BT::NodeStatus onRunning() override;
    void onHalted() override;
};

// Выполнить action'ы узла, на котором стоит робот. Нет action'ов — SUCCESS сразу.
class ExecuteNodeActions : public BT::StatefulActionNode, private RobotAccess {
public:
    using BT::StatefulActionNode::StatefulActionNode;
    static BT::PortsList providedPorts() { return {}; }

    BT::NodeStatus onStart() override;
    BT::NodeStatus onRunning() override;
    void onHalted() override;
};

// Робот на decision point: ждать update, продлевающий base. На время ожидания newBaseRequest.
class WaitForBaseExtension : public BT::StatefulActionNode, private RobotAccess {
public:
    using BT::StatefulActionNode::StatefulActionNode;
    static BT::PortsList providedPorts() { return {}; }

    BT::NodeStatus onStart() override;
    BT::NodeStatus onRunning() override;
    void onHalted() override;
};

// Регистрирует все ноды под ID, которые используются в robot_tree.xml.
void register_action_nodes(BT::BehaviorTreeFactory& factory);




#endif //FLEETMANAGERSYSTEM_ACTION_NODES_HPP
