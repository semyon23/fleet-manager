
#include "include/robot/RoboticVehicleSimulator.hpp"
#include "include/vda/factsheet_builder.hpp"
#include "include/actions/SimulatedAction.hpp"
#include "include/actions/ChargingActions.hpp"
#include "../../shared/include/coro_guard.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/as_tuple.hpp>
#include "spdlog/spdlog.h"
#include <algorithm>

namespace {

    bool has_blocking(const std::vector<vda5050::Action>& actions) {
        return std::any_of(actions.begin(), actions.end(), [](const vda5050::Action& a) {
            return a.blocking_type == vda5050::BlockingType::Soft ||
                   a.blocking_type == vda5050::BlockingType::Hard;
        });
    }

}

RoboticVehicleSimulator::RoboticVehicleSimulator(asio::any_io_executor ex, RobotConfig config, ptrPublishingChannel channel)
    : executor(ex),
    config_(config),
    publisher_(ex, channel, State_, topics_, config.publisher),
    order_manager_(State_, Order_),
    driver_(config.motion),
    action_executor_(State_),
    action_manager_(State_, action_executor_, driver_, order_manager_, publisher_),
    simulation_timer_(executor){

    register_pick_drop(action_executor_, config_.load_handling);
    register_simulated_action(action_executor_);
    register_charging_actions(action_executor_, config_.battery);


    auto factsheet = build_factsheet(config_, action_executor_.supported_types());
    order_manager_.set_supported_actions(factsheet.protocol_features.mobile_robot_actions);
    publisher_.set_factsheet(std::move(factsheet));

    driver_.set_on_node_reached([this](const std::string& node_id){
            this->on_node_reached(node_id);
        });
    init_state();
    spdlog::info("[simulator][{}] Robot initialized", name());
}

RoboticVehicleSimulator::~RoboticVehicleSimulator(){
    spdlog::debug("[simulator][{}] Robot removed", name());
}

void RoboticVehicleSimulator::start(){
    auto self = shared_from_this();
    auto spawn = [&](auto coro) {
        boost::asio::co_spawn(executor,
            [self, coro = std::move(coro)]() mutable -> boost::asio::awaitable<void> {
                co_await std::move(coro);
            },
            boost::asio::detached);
    };
    spawn(publisher_.run_state_publisher_loop());
    spawn(publisher_.run_visu_publisher_loop());
    spawn(run_simulation_loop());
    spdlog::info("[simulator][{}] Robot start", name());
}

void RoboticVehicleSimulator::init_state(){
    vda5050::MobileRobotPosition position;
    position.localized = true;
    position.map_id = config_.initial_state.map_id;
    position.x = config_.initial_state.x;
    position.y = config_.initial_state.y;
    position.theta = config_.initial_state.theta;

    State_.power_supply.state_of_charge = config_.initial_state.battery_soc;
    State_.power_supply.battery_voltage = config_.battery.voltage;
    State_.power_supply.charging = false;

    State_.mobile_robot_position = position;

    State_.header.manufacturer = config_.identity.manufacturer;
    State_.header.serial_number = config_.identity.serial_number;
    State_.header.version = vda5050::kProtocolVersion; // Тут будет "3.0.0"

    State_.operating_mode = vda5050::OperatingMode::Automatic;
    State_.driving = false;

    State_.loads = std::vector<vda5050::Load>{};
    State_.paused = false;
    State_.new_base_request = false;

    vda5050::Map initial_map;
    initial_map.map_id = config_.initial_state.map_id;
    initial_map.map_version = config_.initial_state.map_version;
    initial_map.map_status = vda5050::EnableStatus::Enabled;

    State_.maps = std::vector<vda5050::Map>{initial_map};
}

void RoboticVehicleSimulator::stop(){
    simulation_timer_.cancel();
    publisher_.stop();
    spdlog::info("[simulator][{}] Robot stop", name());
}

std::vector<TopicHandler> RoboticVehicleSimulator::get_topic_routes(){

    std::weak_ptr<RoboticVehicleSimulator> weak_self = shared_from_this();

    auto make_handler = [weak_self](auto method) {
        return [weak_self, method](const std::string& p) {
            if (auto self = weak_self.lock()) {
                asio::post(self->executor, [self, method, p_moved = p]() mutable {
                    ((*self).*method)(std::move(p_moved));
                });
            }
        };
    };

    return {
        { topics_.get_order_topic(),          make_handler(&RoboticVehicleSimulator::handle_order) },
        { topics_.get_instant_actions_topic(), make_handler(&RoboticVehicleSimulator::handle_instant_action) }
    };
}

void RoboticVehicleSimulator::handle_order(const std::string& json){
    order_manager_.update(std::move(json));
}

void RoboticVehicleSimulator::handle_instant_action(const std::string& json){
    action_manager_.handle_instant_actions(std::move(json));
}


void RoboticVehicleSimulator::on_link_up(){
    publisher_.on_link_up();
}

asio::awaitable<void> RoboticVehicleSimulator::run_simulation_loop(){
    using clock = std::chrono::steady_clock;
    auto period = std::chrono::milliseconds(config_.simulation.tick_period);

    auto next = clock::now();
    auto last = next;
    spdlog::info("[simulator][{}] Robot simulation loop start", name());
    CoroGuard guard([this](){
        tree_.haltTree();
        spdlog::info("[simulator][{}] Robot simulation loop stopped", name());
    });
    while (true)
    {
        next += period;
        simulation_timer_.expires_at(next);
        auto [ec] = co_await simulation_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            break;
        }

        const auto now = clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        simulation_tick(dt);
        tree_.tickOnce();
        publisher_.emit_state_changed();
    }
}

void RoboticVehicleSimulator::simulation_tick(double dt){
    dt = std::min(dt, 0.1);

    driver_.update_position(dt);

    auto& pos = State_.mobile_robot_position.emplace(State_.mobile_robot_position.value_or(
        vda5050::MobileRobotPosition{}));
    pos.x = driver_.x();
    pos.y = driver_.y();
    pos.theta = driver_.theta();

    auto& vel = State_.velocity.emplace();
    vel.vx = driver_.speed();
    vel.vy = 0.0;
    vel.omega = driver_.omega();

    State_.driving = std::fabs(vel.vx.value()) > 1e-3 || std::abs(vel.omega.value()) > 1e-3;
    update_battery(dt);
    action_executor_.update(dt);
    action_manager_.update_pending_cancel();

}

void RoboticVehicleSimulator::init_behavior_tree(BT::BehaviorTreeFactory& factory){
    auto blackboard = BT::Blackboard::create();
    // blackboard->set<std::shared_ptr<RobotBehaviorApi>>("robot_api", shared_from_this());
    blackboard->set<RobotBehaviorApi*>("robot_api", this);
    tree_ = factory.createTreeFromFile(config_.behavior_tree.xml_path, blackboard);
    spdlog::info("[{}] The behavior tree has been successfully initialized", name());
}

void RoboticVehicleSimulator::on_node_reached(const std::string& node_id){
    const vda5050::Node* node = find_next_node(node_id);
    if (!node){
        spdlog::error("[simulator][{}] reached node which is not ahead in the current order, ignored", name());
        return;
    }
    const std::uint32_t seq = node->sequence_id;

    // Узлы проходятся строго по порядку. Пропуск означает рассинхрон контроллера и Order.
    const std::uint32_t expected = State_.last_node_id.empty() ? seq : State_.last_node_sequence_id + 2;
    if (seq != expected){
        spdlog::error("[simulator][{}] node {} (seq {}) reached, expected seq {} ", name(), node_id, seq, expected);
    }
    // 1. Съехали с ребра, которое вело к узлу: убрать из edgeStates, завершить его action'ы.
    if (const auto* edge_in = find_edge(seq - 1)) {
        action_executor_.finish(edge_in->actions);
    }
    auto& es = State_.edge_states;
    es.erase(std::remove_if(es.begin(), es.end(),
                            [&](const auto& e) { return e.sequence_id < seq; }),
             es.end());

    auto& ns = State_.node_states;
    ns.erase(std::remove_if(ns.begin(), ns.end(),
                            [&](const auto& n) { return n.sequence_id <= seq; }),
             ns.end());
    State_.last_node_id = node->node_id;
    State_.last_node_sequence_id = seq;

    action_executor_.enqueue(node->actions);

    if (const auto* edge_out = find_edge(seq + 1); edge_out && edge_out->released) {
        action_executor_.enqueue(edge_out->actions);
    }
    spdlog::info("[simulator][{}] node : {}, seq : {}", name(), node_id, seq);
}

const vda5050::Node* RoboticVehicleSimulator::find_next_node(const std::string& node_id) const{
    for (const auto& node : Order_.nodes) {
        if (!node.released) {
            break;
        }
        if (node.sequence_id > State_.last_node_sequence_id && node.node_id == node_id) {
            return &node;
        }
    }
    return nullptr;
}

const vda5050::Edge* RoboticVehicleSimulator::find_edge(std::uint32_t sequence_id){
    for (const auto& edge : Order_.edges) {
        if (edge.sequence_id == sequence_id) {
            return &edge;
        }
    }
    return nullptr;
}

const vda5050::Node* RoboticVehicleSimulator::current_node() const{
    for (const auto& n : Order_.nodes) {
        if (n.sequence_id == State_.last_node_sequence_id && n.node_id == State_.last_node_id) {
            return &n;
        }
    }
    return nullptr;
}

void RoboticVehicleSimulator::update_battery(double dt){
    auto& ps = State_.power_supply;
    const auto& b = config_.battery;
    if (ps.charging && State_.driving) {
        ps.charging = false;
        spdlog::info("[simulator][{}] charging stopped: robot started driving", name());
    }
    if (ps.charging) {
        ps.state_of_charge += b.charge_per_s * dt;
    } else {
        const double meters = std::fabs(driver_.speed()) * dt;
        ps.state_of_charge -= b.drain_per_meter * meters + b.drain_idle_per_s * dt;
    }
    ps.state_of_charge = std::clamp(ps.state_of_charge, 0.0, 100.0);   // <algorithm>
}

bool RoboticVehicleSimulator::is_blocking(const vda5050::Node& node){
    if (has_blocking(node.actions)) {
        return true;
    }
    const auto* edge_out = find_edge(node.sequence_id + 1);
    return edge_out && edge_out->released && has_blocking(edge_out->actions);
}

std::vector<DriveWaypoint> RoboticVehicleSimulator::build_segment(std::uint32_t after_seq){
    std::vector<DriveWaypoint> wps;

    for (const auto& node : Order_.nodes) {
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

        DriveWaypoint wp;
        wp.x = node.node_position->x;
        wp.y = node.node_position->y;
        wp.node_id = node.node_id;
        wp.theta = node.node_position->theta;
        wp.stop = is_blocking(node);

        wps.push_back(std::move(wp));


        this->segment_end_seq_ = node.sequence_id;
        this->segment_ends_blocking_ = wps.back().stop;

        if (this->segment_ends_blocking_) {
            break;
        }
    }
    return wps;
}

std::string RoboticVehicleSimulator::name() const{
    return config_.name();
}

bool RoboticVehicleSimulator::is_safe() const{
    return State_.safety_state.active_emergency_stop == vda5050::EmergencyStop::None && !State_.safety_state.field_violation;
}

bool RoboticVehicleSimulator::is_automatic_mode() const{
    return State_.operating_mode == vda5050::OperatingMode::Automatic ||
          State_.operating_mode == vda5050::OperatingMode::Semiautomatic;
}

bool RoboticVehicleSimulator::is_paused() const{
    return State_.paused.value_or(false);
}

bool RoboticVehicleSimulator::has_active_order() const{
    return !vda5050::is_idle(State_);
}

bool RoboticVehicleSimulator::has_base_ahead() const{
    for (const auto& node : Order_.nodes) {
        if (!node.released) {
            return false;
        }
        if (node.sequence_id > State_.last_node_sequence_id) {
            return true;
        }
    }
    return false;
}

bool RoboticVehicleSimulator::at_decision_point() const{
    return !has_base_ahead() && !State_.node_states.empty();
}

bool RoboticVehicleSimulator::start_segment(){
    start_node_actions();
    if (const auto* edge = find_edge(State_.last_node_sequence_id + 1); edge && edge->released) {
        action_executor_.enqueue(edge->actions);
    }
    if (action_executor_.blocks_driving()) {
        return false;   // SOFT/HARD в работе или в очереди — стоим
    }
    std::vector<DriveWaypoint> wps = build_segment(this->State_.last_node_sequence_id);

    if (wps.empty()) {
        return false;
    }
    driver_.set_path(std::move(wps));
    return true;
}

bool RoboticVehicleSimulator::extend_segment(){

    if (this->segment_ends_blocking_ || driver_.path_finished()) {
        return false;
    }

    const std::uint32_t backup_end_seq = this->segment_end_seq_;
    const bool backup_ends_blocking = this->segment_ends_blocking_;

    std::vector<DriveWaypoint> more = this->build_segment(backup_end_seq);

    if (more.empty()) {
        this->segment_end_seq_ = backup_end_seq;
        this->segment_ends_blocking_ = backup_ends_blocking;
        return false;
    }

    const auto it = std::find_if(Order_.nodes.begin(), Order_.nodes.end(),
                                 [&](const auto& n) { return n.sequence_id == backup_end_seq; });
    if (it != Order_.nodes.end() && it->node_position) {
        DriveWaypoint stitch;
        stitch.x = it->node_position->x;
        stitch.y = it->node_position->y;
        stitch.node_id = it->node_id;
        stitch.theta = it->node_position->theta;
        stitch.stop = false;
        more.insert(more.begin(), std::move(stitch));
    }

    driver_.append_path(std::move(more));

    return true;
}

RobotBehaviorApi::DriveStatus RoboticVehicleSimulator::drive_status() const{
    if (driver_.path_finished() && !driver_.is_moving()) {
        return DriveStatus::Arrived;
    }
    return DriveStatus::Driving;
}

void RoboticVehicleSimulator::stop_driving(){
    driver_.standstill();
}

void RoboticVehicleSimulator::start_node_actions(){
    if (const auto* n = current_node()) action_executor_.enqueue(n->actions);
}

bool RoboticVehicleSimulator::node_actions_done() const{
    // Узел отработан, когда закончились его action'ы и ничего не мешает ехать дальше:
    // SOFT/HARD action ребра, выходящего из узла, выполняется здесь же, стоя.
    if (action_executor_.blocks_driving()) {
        return false;
    }
    const auto* n = current_node();
    return !n || action_executor_.is_done(n->actions);
    // const auto* n = current_node();
    // return !n || action_executor_.is_done(n->actions);
}


void RoboticVehicleSimulator::set_new_base_request(bool value){
    State_.new_base_request = value;
}
