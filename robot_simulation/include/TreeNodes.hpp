#ifndef FLEETMANAGERSYSTEM_TREENODES_HPP
#define FLEETMANAGERSYSTEM_TREENODES_HPP

#include <behaviortree_cpp/behavior_tree.h>
#include "include/RobotSimulator.hpp"


class WaitForOrder : public BT::StatefulActionNode {
public:
    WaitForOrder(const std::string& name, const BT::NodeConfig& config);

    static BT::PortsList providedPorts() { return {}; }

    BT::NodeStatus onStart() override;
    BT::NodeStatus onRunning() override;
    void onHalted() override;

private:
    std::shared_ptr<RobotSimulator> robot;
};

class OrderFullyCompleted : public BT::SyncActionNode{
public:
    OrderFullyCompleted(const std::string& name, const BT::NodeConfig& config);
    static BT::PortsList providedPorts() { return {}; }
    BT::NodeStatus tick() override;
};

class GetNextSegment : public BT::SyncActionNode {
public:
    GetNextSegment(const std::string& name, const BT::NodeConfig& config);
    static BT::PortsList providedPorts() {
        return {
            BT::OutputPort<double>("target_x"),
            BT::OutputPort<double>("target_y"),
            BT::OutputPort<std::string>("edge_actions"),
            BT::OutputPort<std::string>("node_actions")
        };
    }
    BT::NodeStatus tick() override;
};

class CheckOrderValid : public BT::ConditionNode {
public:
    CheckOrderValid(const std::string& name, const BT::NodeConfig& config);
    static BT::PortsList providedPorts() { return {}; }
    BT::NodeStatus tick() override;
};


class MarkSegmentComplete : public BT::SyncActionNode {
public:
    MarkSegmentComplete(const std::string& name, const BT::NodeConfig& config);
    static BT::PortsList providedPorts() { return {}; }
    BT::NodeStatus tick() override;
};


class MoveToPose : public BT::StatefulActionNode {
public:
    MoveToPose(const std::string& name, const BT::NodeConfig& config);
    static BT::PortsList providedPorts() {
        return { BT::InputPort<double>("x"), BT::InputPort<double>("y") };
    }
    BT::NodeStatus onStart() override;
    BT::NodeStatus onRunning() override;
    void onHalted() override;
private:
    std::shared_ptr<RobotSimulator> robot;
    double target_x_ = 0.0;
    double target_y_ = 0.0;
};

class ExecuteVdaActions : public BT::StatefulActionNode {
public:
    ExecuteVdaActions(const std::string& name, const BT::NodeConfig& config);
    static BT::PortsList providedPorts() {
        return {
            BT::InputPort<std::string>("actions"),
            BT::InputPort<std::string>("trigger")
        };
    }
    BT::NodeStatus onStart() override;
    BT::NodeStatus onRunning() override;
    void onHalted() override;
private:
    std::shared_ptr<RobotSimulator> robot_;
    std::string actions_json_;
    std::string trigger_type_;
};


#endif //FLEETMANAGERSYSTEM_TREENODES_HPP
