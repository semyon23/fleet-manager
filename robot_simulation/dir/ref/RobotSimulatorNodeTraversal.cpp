// RobotSimulator: прохождение узлов маршрута (VDA 5050 v3.0.0, 6.6.2).
//
// on_node_reached() передаётся в контроллер движения:
//     drive_controller.set_on_node_reached([this](const std::string& id) { on_node_reached(id); });
// Контроллер вызывает его из update_position(), то есть в потоке симуляции.

#include "RobotSimulator.hpp"

#include <algorithm>
#include <iostream>

void RobotSimulator::on_node_reached(const std::string& node_id) {
    const vda5050::Node* node = find_next_node(node_id);
    if (!node) {
        // Order уже отменён/заменён, или контроллер едет не по Order — State не трогаем.
        std::cerr << "[Robot] reached node '" << node_id
                  << "' which is not ahead in the current order, ignored\n";
        return;
    }
    const std::uint32_t seq = node->sequence_id;

    // Узлы проходятся строго по порядку. Пропуск означает рассинхрон контроллера и Order.
    const std::uint32_t expected = State.last_node_id.empty() ? seq : State.last_node_sequence_id + 2;
    if (seq != expected) {
        std::cerr << "[Robot] node '" << node_id << "' (seq " << seq << ") reached, expected seq "
                  << expected << "\n";
    }

    // 1. Съехали с ребра, которое вело к узлу: убрать из edgeStates, завершить его action'ы.
    if (const auto* edge_in = find_edge(seq - 1)) {
        actions_.finish(edge_in->actions);
    }
    auto& es = State.edge_states;
    es.erase(std::remove_if(es.begin(), es.end(),
                            [&](const auto& e) { return e.sequence_id < seq; }),
             es.end());

    // 2. Узел пройден: убрать из nodeStates, обновить lastNodeId / lastNodeSequenceId.
    auto& ns = State.node_states;
    ns.erase(std::remove_if(ns.begin(), ns.end(),
                            [&](const auto& n) { return n.sequence_id <= seq; }),
             ns.end());
    State.last_node_id = node->node_id;
    State.last_node_sequence_id = seq;

    // 3. Поставить action'ы узла в очередь исполнителя. Блокирующие (SOFT/HARD) —
    //    это точки остановки, их завершения ждёт нода дерева ExecuteNodeActions.
    actions_.enqueue(node->actions);

    // 4. Въехали на следующее ребро (если оно released) — запустить его action'ы.
    //    Если на узле есть SOFT/HARD action, въезд на ребро будет, когда робот тронется (6.6.2);
    //    для простоты симулятора запускаем сразу.
    if (const auto* edge_out = find_edge(seq + 1); edge_out && edge_out->released) {
        actions_.enqueue(edge_out->actions);
    }

    std::cout << "[Robot] node '" << node_id << "' (seq " << seq << ") traversed\n";
    emit_state_changed();
}

// Первый ещё не пройденный released-узел с этим id.
// Id может встречаться в маршруте несколько раз, поэтому ищем только впереди робота.
const vda5050::Node* RobotSimulator::find_next_node(const std::string& node_id) const {
    for (const auto& node : Order.nodes) {
        if (!node.released) {
            break;  // дальше horizon — туда робот ехать не мог
        }
        if (node.sequence_id > State.last_node_sequence_id && node.node_id == node_id) {
            return &node;
        }
    }
    return nullptr;
}

const vda5050::Edge* RobotSimulator::find_edge(std::uint32_t sequence_id) const {
    for (const auto& edge : Order.edges) {
        if (edge.sequence_id == sequence_id) {
            return &edge;
        }
    }
    return nullptr;
}
