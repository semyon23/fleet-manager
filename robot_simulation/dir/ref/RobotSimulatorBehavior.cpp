// RobotSimulator: реализация RobotBehaviorApi (action_nodes.hpp) — то, что спрашивает
// и чем управляет дерево поведения. Всё вызывается из потока симуляции (tree.tickOnce()).

#include "RobotSimulator.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

bool is_blocking(const vda5050::Node& node) {
    return std::any_of(node.actions.begin(), node.actions.end(), [](const vda5050::Action& a) {
        return a.blocking_type == vda5050::BlockingType::Soft ||
               a.blocking_type == vda5050::BlockingType::Hard;
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------

std::string RobotSimulator::name() const {
    return config.name;
}

bool RobotSimulator::is_safe() const {
    return State.safety_state.active_emergency_stop == vda5050::EmergencyStop::None &&
           !State.safety_state.field_violation;
}

bool RobotSimulator::is_automatic_mode() const {
    return State.operating_mode == vda5050::OperatingMode::Automatic ||
           State.operating_mode == vda5050::OperatingMode::Semiautomatic;
}

bool RobotSimulator::is_paused() const {
    return State.paused.value_or(false);
}

bool RobotSimulator::has_active_order() const {
    return !vda5050::is_idle(State);
}

bool RobotSimulator::has_base_ahead() const {
    for (const auto& node : Order.nodes) {
        if (!node.released) {
            return false;
        }
        if (node.sequence_id > State.last_node_sequence_id) {
            return true;
        }
    }
    return false;
}

bool RobotSimulator::at_decision_point() const {
    // Released впереди нет, но в nodeStates остались узлы horizon.
    return !has_base_ahead() && !State.node_states.empty();
}

// ---------------------------------------------------------------------------
// Движение
// ---------------------------------------------------------------------------

// Путь по released-узлам после after_seq до ближайшей точки остановки:
// узла с SOFT/HARD action или конца base. Запоминает, где сегмент кончается.
std::vector<Waypoint> RobotSimulator::build_segment(std::uint32_t after_seq) {
    std::vector<Waypoint> wps;
    for (const auto& node : Order.nodes) {
        if (!node.released) {
            break;
        }
        if (node.sequence_id <= after_seq) {
            continue;
        }
        if (!node.node_position) {
            std::cerr << "[Robot] node '" << node.node_id << "' has no nodePosition, segment cut\n";
            break;
        }
        Waypoint wp;
        wp.x = node.node_position->x;
        wp.y = node.node_position->y;
        wp.node_id = node.node_id;
        wp.theta = node.node_position->theta;
        wp.stop = is_blocking(node);
        wps.push_back(std::move(wp));

        segment_end_seq_ = node.sequence_id;
        segment_ends_blocking_ = wps.back().stop;
        if (segment_ends_blocking_) {
            break;  // на этом узле робот обязан остановиться
        }
    }
    return wps;
}

bool RobotSimulator::start_segment() {
    // SOFT/HARD action в работе или в очереди — ехать нельзя (6.2.2).
    if (actions_.blocks_driving()) {
        return false;
    }
    auto wps = build_segment(State.last_node_sequence_id);
    if (wps.empty()) {
        return false;
    }
    drive_controller.set_path(std::move(wps));  // последняя точка — всегда остановка
    return true;
}

bool RobotSimulator::extend_segment() {
    // Сегмент кончается узлом с блокирующим action — там стоим в любом случае.
    // Робот уже доехал — продлит следующий DriveSegment.
    if (segment_ends_blocking_ || drive_controller.path_finished()) {
        return false;
    }
    const std::uint32_t old_end = segment_end_seq_;
    auto more = build_segment(old_end);
    if (more.empty()) {
        return false;
    }
    // append_path ждёт первой точкой бывший конец пути (stitching), с stop = false:
    // тогда контроллер не тормозит на нём.
    const auto it = std::find_if(Order.nodes.begin(), Order.nodes.end(),
                                 [&](const auto& n) { return n.sequence_id == old_end; });
    if (it != Order.nodes.end() && it->node_position) {
        Waypoint stitch;
        stitch.x = it->node_position->x;
        stitch.y = it->node_position->y;
        stitch.node_id = it->node_id;
        stitch.stop = false;
        more.insert(more.begin(), std::move(stitch));
    }
    drive_controller.append_path(std::move(more));
    return true;
}

RobotBehaviorApi::DriveStatus RobotSimulator::drive_status() const {
    // Доехал = путь пуст И робот реально остановился, а не просто прошёл последнюю точку.
    if (drive_controller.path_finished() && std::abs(drive_controller.speed()) < 1e-3) {
        return DriveStatus::Arrived;
    }
    return DriveStatus::Driving;  // Failed появится, когда будет Nav2 / детектор застревания
}

void RobotSimulator::stop_driving() {
    drive_controller.standstill();
}

// ---------------------------------------------------------------------------
// Action'ы узла, на котором стоит робот
// ---------------------------------------------------------------------------

const vda5050::Node* RobotSimulator::current_node() const {
    for (const auto& node : Order.nodes) {
        if (node.sequence_id == State.last_node_sequence_id) {
            return &node;
        }
    }
    return nullptr;
}

void RobotSimulator::start_node_actions() {
    // Обычно уже в очереди после on_node_reached(); здесь — для первого узла нового order'а,
    // на котором робот стоял с самого начала. enqueue() пропускает уже запущенные.
    if (const auto* node = current_node()) {
        actions_.enqueue(node->actions);
    }
}

bool RobotSimulator::node_actions_done() const {
    const auto* node = current_node();
    return !node || actions_.is_done(node->actions);
}

void RobotSimulator::cancel_node_actions() {
    if (const auto* node = current_node()) {
        actions_.cancel(node->actions);
    }
}

void RobotSimulator::set_new_base_request(bool value) {
    State.new_base_request = value;
}
