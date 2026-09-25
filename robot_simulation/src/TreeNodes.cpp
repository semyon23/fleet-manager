
#include "../include/TreeNodes.hpp"
#include <print>

WaitForOrder::WaitForOrder(const std::string& name, const BT::NodeConfig& config)
    : BT::StatefulActionNode(name, config) {}

BT::NodeStatus WaitForOrder::onStart() {

    if (!config().blackboard->get("robot_instance", robot)) {
        return BT::NodeStatus::FAILURE;
    }

    std::println("[BT] {} Robot started waiting for a new order...",  robot->get_name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus WaitForOrder::onRunning() {
    if (false) {
        std::println("[BT] {} Robot -> WaitForOrder : SUCCESS (Order received!)",  robot->get_name());
        return BT::NodeStatus::SUCCESS;
    }
    std::println("[BT] {} Robot -> WaitForOrder : RUNNING ",  robot->get_name());
    return BT::NodeStatus::RUNNING;
}

void WaitForOrder::onHalted() {
    std::cout << "[BT] WaitForOrder halted for robot " << robot->get_name() << std::endl;
}


GetNextSegment::GetNextSegment(const std::string& name, const BT::NodeConfig& config)
    : BT::SyncActionNode(name, config) {}

BT::NodeStatus GetNextSegment::tick() {
    std::shared_ptr<RobotSimulator> robot;

    if (!config().blackboard->get("robot_instance", robot)) {
        return BT::NodeStatus::FAILURE;
    }

    static int tick = 0;
    tick++;
    if (tick> 2) {//!robot->has_more_segments()
        std::println("GetNextSegment : FAILURE");
        return BT::NodeStatus::FAILURE;
    }

    double x = tick + 5, y = tick + 3;
    std::string edge_act, node_act;
  //  robot->get_next_segment_data(x, y, edge_act, node_act);

    setOutput("target_x", x);
    setOutput("target_y", y);
    setOutput("edge_actions", edge_act);
    setOutput("node_actions", node_act);


    std::println("GetNextSegment : SUCCESS");
    return BT::NodeStatus::SUCCESS;

}


OrderFullyCompleted::OrderFullyCompleted(const std::string& name, const BT::NodeConfig& config)
    : BT::SyncActionNode(name, config) {}

BT::NodeStatus OrderFullyCompleted::tick(){
    std::shared_ptr<RobotSimulator> robot;
    if (!config().blackboard->get("robot_instance", robot)) {
        return BT::NodeStatus::FAILURE;
    }

    config().blackboard->set("x", 0.0);
    config().blackboard->set("y", 0.0);
    config().blackboard->set("edge_actions", std::string(""));
    config().blackboard->set("node_actions", std::string(""));

    std::println("OrderFullyCompleted : SUCCESS");
    return BT::NodeStatus::SUCCESS;
}


CheckOrderValid::CheckOrderValid(const std::string& name, const BT::NodeConfig& config)
    : BT::ConditionNode(name, config) {}

BT::NodeStatus CheckOrderValid::tick() {
    std::shared_ptr<RobotSimulator> robot;
    if (!config().blackboard->get("robot_instance", robot)) {
        return BT::NodeStatus::FAILURE;
    }

    static int tick = 0;
    tick++;
    // Проверяем, не пришел ли по сети order с флагом отмены (VDA 5050 cancelOrder)
    if (tick>100) { // robot->is_order_canceled()
        std::cout << "[BT] Order was canceled by master control!" << std::endl;
        std::println("CheckOrderValid : FAILURE");
        return BT::NodeStatus::FAILURE; // Прервет ReactiveSequence и остановит движение
    }
    std::println("CheckOrderValid : SUCCESS");
    return BT::NodeStatus::SUCCESS;

}

// --- 4. MoveToPose (Асинхронный) ---
MoveToPose::MoveToPose(const std::string& name, const BT::NodeConfig& config)
    : BT::StatefulActionNode(name, config) {}

BT::NodeStatus MoveToPose::onStart() {
    if (!config().blackboard->get("robot_instance", robot)) {
        return BT::NodeStatus::FAILURE; // Если робота забыли положить в Blackboard
    }

    if (!getInput("x", target_x_) || !getInput("y", target_y_)) {
        return BT::NodeStatus::FAILURE;
    }

    std::cout << "[BT] Robot " << robot->get_name()
              << " starting move to (" << target_x_ << ", " << target_y_ << ")" << std::endl;
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus MoveToPose::onRunning() {
    //robot->move_to(); // Крутим физику/кинематику перемещения вашего робота
    static int tick = 0;
    tick++;

    if (tick > 20) { // robot->is_target_reached(target_x_, target_y_)
        std::println("MoveToPose : SUCCESS");
        return BT::NodeStatus::SUCCESS;
    }
    std::println("MoveToPose : RUNNING");
    return BT::NodeStatus::RUNNING; // Продолжаем движение на следующем тике дерева
}

void MoveToPose::onHalted() {
    // Вызывается автоматически, если CheckOrderValid вернул FAILURE
    robot->standstill(); // Экстренно останавливаем симуляцию привода робота
  //  std::cout << "[BT] Movement halted due to order cancellation!" << std::endl;
}


// --- 5. ExecuteVdaActions (Асинхронный) ---
ExecuteVdaActions::ExecuteVdaActions(const std::string& name, const BT::NodeConfig& config)
    : BT::StatefulActionNode(name, config) {}

BT::NodeStatus ExecuteVdaActions::onStart() {
    if (!config().blackboard->get("robot_instance", robot_)) {
        return BT::NodeStatus::FAILURE; // Если робота забыли положить в Blackboard
    }

    if (!getInput("actions", actions_json_) || !getInput("trigger", trigger_type_)) {
        return BT::NodeStatus::FAILURE;
    }
    std::println("Execute Actions  : onStart {}", robot_->get_name());
    return BT::NodeStatus::RUNNING;
}

BT::NodeStatus ExecuteVdaActions::onRunning() {

    if (trigger_type_ == "DURING_MOVEMENT") {
        // Например, VDA 5050 требует мигать лампами или пищать при движении
       // robot_->execute_action();

        // Действия во время движения обычно завершаются вместе с движением,
        // поэтому возвращаем SUCCESS сразу, чтобы параллельный узел не блокировал MoveToPose
        std::println("Execute Actions AFTER_ARRIVE : SUCCESS {}", robot_->get_name());
        return BT::NodeStatus::SUCCESS;
    }
    else if (trigger_type_ == "AFTER_ARRIVE") {
        std::println("Execute Actions DURING_MOVEMENT : SUCCESS {}", robot_->get_name());
        return BT::NodeStatus::SUCCESS;
        if (true) {// robot_->is_action_complete()

        }
    }
    return BT::NodeStatus::RUNNING;
}

void ExecuteVdaActions::onHalted() {
    // Если пришла отмена заказа — сбрасываем выполнение текущего экшна
    robot_->standstill();
}

// --- 6. MarkSegmentComplete (Синхронный) ---
MarkSegmentComplete::MarkSegmentComplete(const std::string& name, const BT::NodeConfig& config)
    : BT::SyncActionNode(name, config) {}

BT::NodeStatus MarkSegmentComplete::tick() {
    std::shared_ptr<RobotSimulator> robot;
    //config().blackboard->get("robot_instance", robot);
    if (!config().blackboard->get("robot_instance", robot)) {
        return BT::NodeStatus::FAILURE; // Если робота забыли положить в Blackboard
    }
    // Робот внутри себя фиксирует, что сегмент закрыт, обновляет lastNodeId
    // и готовит новый пакет данных для отправки в топик state
   // robot->mark_current_segment_done();

    std::println("MarkSegmentComplete : SUCCESS {}", robot->get_name());
    return BT::NodeStatus::SUCCESS;
}

