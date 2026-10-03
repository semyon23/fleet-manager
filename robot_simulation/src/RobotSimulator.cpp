//#include <utility>

#include "../include/RobotSimulator.hpp"
#include <chrono>
#include <format>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include "include/RobotBehaviorImpl.hpp"
#include <spdlog/spdlog.h>
#include "../../shared/include/coro_guard.hpp"


RobotSimulator::RobotSimulator(RobotConfig conf, Pub_channel_ptr channel)
    : config(conf),
    pub_channel(std::move(channel)),
    drive_controller(conf.drive_params){
    this->init_state();
    this->init_factsheet();
    spdlog::info("[{}] Robot creating", get_name());
};

RobotSimulator::~RobotSimulator(){
    spdlog::info("[{}] Robot destroyed", get_name());
}

void RobotSimulator::init_behavior_tree(BT::BehaviorTreeFactory& factory, const std::string& xml_path){

    blackboard = BT::Blackboard::create();
    auto behavior_api = std::make_shared<RobotBehaviorImpl>(shared_from_this());
    blackboard->set<std::shared_ptr<RobotBehaviorApi>>("robot_api", behavior_api);
    tree = factory.createTreeFromFile(xml_path, blackboard);
    // 6. Опционально: Раскомментируйте для визуализации дерева в Groot2
   // groot = std::make_unique<BT::Groot2Publisher>(tree, groot_port);
    // std::cout << "[BT] Groot2 publisher started on port " << groot_port << std::endl;

    spdlog::info("[{}] The behavior tree has been successfully initialized", get_name());
}

void RobotSimulator::start(asio::any_io_executor executor){
    simulation_timer = std::make_unique<asio::steady_timer>(executor);
    state_trigger_channel = std::make_unique<asio::experimental::channel<void(boost::system::error_code, bool)>>(executor, 1);
    auto self = shared_from_this();
    boost::asio::co_spawn(executor, [self]() { return self->run_simulation_loop(); }, boost::asio::detached);
    boost::asio::co_spawn(executor, [self]() { return self->run_visu_publisher_loop(); }, boost::asio::detached);
    boost::asio::co_spawn(executor, [self]() { return self->run_state_publisher_loop(); }, boost::asio::detached);
}

void RobotSimulator::stop(){
    stopping = true;
    simulation_timer->cancel();
    state_trigger_channel->cancel();

    this->blackboard->set("robot_api", std::shared_ptr<RobotBehaviorApi>());
    this->tree = BT::Tree();
}


void RobotSimulator::init_state(){
    for (vda5050::Header* h : {&State.header, &Connection.header, &Visualization.header}) {
        h->manufacturer = config.manufacturer;
        h->serial_number = config.name;
        h->version = vda5050::kProtocolVersion;
    }

    min_state_interval_ = config.min_state_interval_;
    max_state_interval_ = config.max_state_interval_;

    const std::string base_topic = std::format("amr/v3/{}/{}/", config.manufacturer, config.name);
    state_topic              = base_topic + "state";
    visu_topic               = base_topic + "visualization";
    connection_topic         = base_topic + "connection";
    factsheet_topic          = base_topic + "factsheet";
    sub_order_topic          = base_topic + "order";
    sub_instantAction_topic  = base_topic + "instantActions";

    Connection.connection_state = vda5050::ConnectionState::Online;

    State.operating_mode = vda5050::OperatingMode::Automatic;
    State.maps = std::vector<vda5050::Map>{
        {config.map, "1", vda5050::EnableStatus::Enabled, std::nullopt}};

    auto& pos = State.mobile_robot_position.emplace();
    pos.x = 0.0;
    pos.y = 0.0;
    pos.theta = 0.0;
    pos.map_id = config.map;
    pos.localized = true;

    State.velocity.emplace();
    Visualization.mobile_robot_position = State.mobile_robot_position;
    Visualization.velocity = State.velocity;

    State.power_supply.state_of_charge = 100.0;   // [%]
    State.power_supply.battery_voltage = 48.0;    // [V]
    State.power_supply.battery_current = 0.0;     // [A], робот стоит
    State.power_supply.battery_health = 100;      // [%]
    State.power_supply.charging = false;

    drive_controller.set_on_node_reached([this](const std::string& node_id)
        {
            this->on_node_reached(node_id);
        });

    spdlog::info("[{}] Robot initialized", get_name());

}


void RobotSimulator::simulation_tick(double dt){
    dt = std::min(dt, 0.1);

    drive_controller.update_position(dt);

    auto& pos = State.mobile_robot_position.emplace(State.mobile_robot_position.value_or(
        vda5050::MobileRobotPosition{}));
    pos.x = drive_controller.x();
    pos.y = drive_controller.y();
    pos.theta = drive_controller.theta();

    auto& vel = State.velocity.emplace();
    vel.vx = drive_controller.speed();
    vel.vy = 0.0;
    vel.omega = drive_controller.omega();

    State.driving = std::fabs(vel.vx.value()) > 1e-3 || std::abs(vel.omega.value()) > 1e-3;

    Visualization.mobile_robot_position = State.mobile_robot_position;
    Visualization.velocity = State.velocity;
}

void RobotSimulator::on_node_reached(const std::string& node_id){
    const vda5050::Node* node = find_next_node(node_id);
    if (!node) {
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
            finish_edge_actions(*edge_in);
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

        // 3. Запустить action'ы узла. Блокирующие (SOFT/HARD) — это точки остановки,
        //    их завершения ждёт нода дерева ExecuteNodeActions.
        trigger_actions(node->actions);

        // 4. Въехали на следующее ребро (если оно released) — запустить его action'ы.
        //    Если на узле есть SOFT/HARD action, въезд на ребро будет, когда робот тронется (6.6.2);
        //    для простоты симулятора запускаем сразу.
        if (const auto* edge_out = find_edge(seq + 1); edge_out && edge_out->released) {
            trigger_actions(edge_out->actions);
        }

        //std::cout << "[Robot] node '" << node_id << "' (seq " << seq << ") traversed\n";
        spdlog::info("[{}] node : {}, seq : {}", get_name(), node_id, seq);
}

void RobotSimulator::finish_edge_actions(const vda5050::Edge& edge){
    using vda5050::ActionStatus;
    for (const auto& action : edge.actions) {
        auto* st = find_action_state(action.action_id);
        if (!st || st->action_status == ActionStatus::Finished ||
            st->action_status == ActionStatus::Failed) {
            continue;
            }
        // Запускался — завершён; так и не начался — не выполнен.
        st->action_status = st->action_status == ActionStatus::Waiting ? ActionStatus::Failed
                                                                        : ActionStatus::Finished;
    }
}

vda5050::ActionState* RobotSimulator::find_action_state(const std::string& action_id){
    for (auto& a : State.action_states) {
        if (a.action_id == action_id) {
            return &a;
        }
    }
    return nullptr;
}

void RobotSimulator::trigger_actions(const std::vector<vda5050::Action>& actions){
    for (const auto& action : actions) {
        auto* st = find_action_state(action.action_id);
        if (!st || st->action_status != vda5050::ActionStatus::Waiting) {
            continue;  // уже запущен или завершён
        }
        std::cout << "[Robot] action '" << action.action_type << "' (" << action.action_id
                  << ") executed\n";
        st->action_status = vda5050::ActionStatus::Finished;
    }
}

const vda5050::Edge* RobotSimulator::find_edge(std::uint32_t sequence_id){
    for (const auto& edge : Order.edges) {
        if (edge.sequence_id == sequence_id) {
            return &edge;
        }
    }
    return nullptr;
}

const vda5050::Node* RobotSimulator::find_next_node(const std::string& node_id) const{
    for (const auto& node : Order.nodes) {
        if (!node.released) {
            break;
        }
        if (node.sequence_id > State.last_node_sequence_id && node.node_id == node_id) {
            return &node;
        }
    }
    return nullptr;
}

void RobotSimulator::emit_state_changed(){
    has_active_order_ = !vda5050::is_idle(State);
    if (state_sent_once && State == Last_sent_state) {
        return;
    }
    if (!state_trigger_channel || !state_trigger_channel->is_open()) {
        std::cerr << "[State] Trigger channel is not ready or closed\n";
        return;
    }

    bool success = state_trigger_channel->try_send(boost::system::error_code{}, true);

    if (!success) {
        std::cerr << "The trigger did not activate (Buffer is full)." << std::endl;
    }
}

asio::awaitable<void> RobotSimulator::run_simulation_loop(){
    using clock = std::chrono::steady_clock;
    constexpr auto period = std::chrono::milliseconds(100);

    auto next = clock::now();
    auto last = next;
    spdlog::info("[{}] Run simulation loop start", get_name());
    CoroGuard guard([this](){
        tree.haltTree();
        spdlog::info("[{}] Run simulation loop stopped", get_name());
    });
    while (true) {
        next += period;
        simulation_timer->expires_at(next);
        auto [ec] = co_await simulation_timer->async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec || stopping) {
            break;
        }

        const auto now = clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        simulation_tick(dt);
        has_active_order_ = !vda5050::is_idle(State);
        tree.tickOnce();
        emit_state_changed();
    }
}

void RobotSimulator::on_link_up(){
    if (!pub_channel->is_open()) return;
    auto connect = this->pub_connection_topic();
    pub_channel->try_send(boost::system::error_code{}, connect);

    auto factsheet = this->pub_factsheet_topic();
    pub_channel->try_send(boost::system::error_code{}, factsheet);
}

asio::awaitable<void> RobotSimulator::run_visu_publisher_loop()
{
    asio::steady_timer timer(co_await asio::this_coro::executor);
    spdlog::info("[{}] Run visu publisher loop start", get_name());
    while (true) {
        timer.expires_after(std::chrono::milliseconds(200));
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));

        if (ec) {
            co_return;
        }
        if (!pub_channel->is_open()){
            co_return;
        }
        auto visu_packet = this->pub_visu_topic();
        bool success=  pub_channel->try_send(boost::system::error_code{}, visu_packet);
        if (!success) {
            co_return;
        }
    }
}

asio::awaitable<void> RobotSimulator::run_state_publisher_loop(){
    auto executor = co_await boost::asio::this_coro::executor;
    asio::steady_timer periodic_timer{executor};

    while (true) {

        if (stopping) { co_return;}

        periodic_timer.expires_after(config.max_state_interval_);

        auto [order, err_timer, err_channel, state_received] = co_await boost::asio::experimental::make_parallel_group(
            periodic_timer.async_wait(boost::asio::deferred),
            state_trigger_channel->async_receive(boost::asio::deferred)
        ).async_wait(boost::asio::experimental::wait_for_one(), asio::use_awaitable);

        size_t first_completed = order[0];

        if (first_completed == 0) {
            if (err_timer) co_return;
        }
        else if (first_completed == 1) {
            if (err_channel) co_return;
            if (!state_received) co_return;
        }

        if (!pub_channel || !pub_channel->is_open() || stopping) co_return;

        auto state_packet = this->pub_state_topic();
        bool sent = pub_channel->try_send(boost::system::error_code{}, state_packet);
        if (!sent) co_return;

        periodic_timer.expires_after(config.min_state_interval_);
        auto [ec] = co_await periodic_timer.async_wait(asio::as_tuple(asio::use_awaitable));

        if (ec) co_return;
    }
}


vda5050::Order RobotSimulator::get_last_received_order() const{
    return Order;
}

vda5050::State& RobotSimulator::get_state(){
    return State;
}

std::string RobotSimulator::get_name() const{
    return config.name;
};

void RobotSimulator::set_new_path(std::vector<DriveWaypoint> wps){
    drive_controller.set_path(std::move(wps));
}

bool RobotSimulator::path_finished() const{
    return drive_controller.path_finished();
}

void RobotSimulator::append_path(std::vector<DriveWaypoint> wps){
    drive_controller.append_path(std::move(wps));
}

bool RobotSimulator::is_blocking(const vda5050::Node& node){

    for (auto it = node.actions.begin(); it != node.actions.end(); ++it) {
        const vda5050::Action& a = *it;

        if (a.blocking_type == vda5050::BlockingType::Soft ||
            a.blocking_type == vda5050::BlockingType::Hard) {
            return true;
            }
    }
    return false;
    // return std::any_of(node.actions.begin(), node.actions.end(), [](const vda5050::Action& a) {
    //         return a.blocking_type == vda5050::BlockingType::Soft ||
    //                a.blocking_type == vda5050::BlockingType::Hard;
    //     });
}

std::vector<DriveWaypoint> RobotSimulator::build_segment(std::uint32_t after_seq){
    std::vector<DriveWaypoint> wps;

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

        DriveWaypoint wp;
        wp.x = node.node_position->x;
        wp.y = node.node_position->y;
        wp.node_id = node.node_id;
        wp.theta = node.node_position->theta;
        wp.stop = is_blocking(node); // Использует std::any_of, который мы разобрали

        wps.push_back(std::move(wp));

        // Фиксируем продвижение в полях класса RobotSimulator
        this->segment_end_seq_ = node.sequence_id;
        this->segment_ends_blocking_ = wps.back().stop;

        // Если уперлись в блокирующий экшн — этот чанк закончен, выходим
        if (this->segment_ends_blocking_) {
            break;
        }
    }
    return wps;
}

bool RobotSimulator::start_new_segment(){
    // Строим чанк, начиная от последней подтвержденной сервером (пройденной) вершины
    // Вызов build_segment(с_какой_точки) сам внутри заполнит segment_end_seq_ и segment_ends_blocking_
    std::vector<DriveWaypoint> wps = this->build_segment(this->State.last_node_sequence_id);

    if (wps.empty()) {
        return false;
    }

    // Загружаем новый путь в контроллер движения (очищая старый)
    this->drive_controller.set_path(std::move(wps));
    return true;
}

bool RobotSimulator::extend_current_segment(){
    // 1. Проверяем флаги симулятора: если чанк завершается блокирующим действием
    //    или робот уже физически закончил путь — продлевать на лету нельзя.
    if (this->segment_ends_blocking_ || this->drive_controller.path_finished()) {
        return false;
    }

    // 2. Бэкапим старые параметры на случай, если новых released-точек в заказе нет
    const std::uint32_t backup_end_seq = this->segment_end_seq_;
    const bool backup_ends_blocking = this->segment_ends_blocking_;

    // 3. Вызов build_segment ПЕРЕЗАПИШЕТ segment_end_seq_ и segment_ends_blocking_ новыми данными
    std::vector<DriveWaypoint> more = this->build_segment(backup_end_seq);

    if (more.empty()) {
        // ОТКАТ: Новых точек нет (или они еще не released), возвращаем старое состояние назад!
        this->segment_end_seq_ = backup_end_seq;
        this->segment_ends_blocking_ = backup_ends_blocking;
        return false;
    }

    // 4. Сшивание путей (Stitching)
    const auto it = std::find_if(Order.nodes.begin(), Order.nodes.end(),
                                 [&](const auto& n) { return n.sequence_id == backup_end_seq; });
    if (it != Order.nodes.end() && it->node_position) {
        DriveWaypoint stitch;
        stitch.x = it->node_position->x;
        stitch.y = it->node_position->y;
        stitch.node_id = it->node_id;
        stitch.theta = it->node_position->theta;
        stitch.stop = false; // Важно: летим старый конец без остановки!
        more.insert(more.begin(), std::move(stitch));
    }

    // 5. Безопасно дописываем склеенный путь в хвост контроллеру
    this->drive_controller.append_path(std::move(more));

    return true;
}

bool RobotSimulator::is_moving() const{
    return this->drive_controller.is_moving();
}

RobotMessage RobotSimulator::pub_connection_topic(){
    Connection.header.header_id++;
    Connection.header.timestamp =  vda5050::make_timestamp();
    RobotMessage msg{connection_topic, vda5050::json(Connection).dump(), 1, true};
    return msg;
}

RobotMessage RobotSimulator::pub_factsheet_topic(){
    Factsheet.header.header_id++;
    Factsheet.header.timestamp = vda5050::make_timestamp();
    RobotMessage msg{factsheet_topic, vda5050::json(Factsheet).dump(), 0, true};
    return msg;
}

RobotMessage RobotSimulator::pub_visu_topic(){
    Visualization.header.header_id++;
    Visualization.header.timestamp =  vda5050::make_timestamp();
    RobotMessage msg{visu_topic, vda5050::json(Visualization).dump()};
    return msg;
}




RobotMessage RobotSimulator::pub_state_topic(){
    State.header.header_id++;
    State.header.timestamp = vda5050::make_timestamp();

    RobotMessage msg{state_topic, vda5050::json(State).dump()};
    Visualization.reference_state_header_id = State.header.header_id;

    Last_sent_state = State;
    state_sent_once = true;
    return msg;
};

void RobotSimulator::start_action(){

}

void RobotSimulator::execute_action(){

}

bool RobotSimulator::has_active_order() const{
    return has_active_order_.load();
}


bool RobotSimulator::is_idle() const{
    using vda5050::ActionStatus;
    return State.node_states.empty() && State.edge_states.empty() &&
           std::all_of(State.action_states.begin(), State.action_states.end(), [](const auto& a) {
               return a.action_status == ActionStatus::Finished ||
                      a.action_status == ActionStatus::Failed;
           });
}

void RobotSimulator::drop_horizon(){
    std::set<std::string> horizon_action_ids;
    for (const auto& n : Order.nodes)
        if (!n.released)
            for (const auto& a : n.actions) horizon_action_ids.insert(a.action_id);
    for (const auto& e : Order.edges)
        if (!e.released)
            for (const auto& a : e.actions) horizon_action_ids.insert(a.action_id);

    auto unreleased = [](const auto& x) { return !x.released; };
    Order.nodes.erase(std::remove_if(Order.nodes.begin(), Order.nodes.end(), unreleased),
                            Order.nodes.end());
    Order.edges.erase(std::remove_if(Order.edges.begin(), Order.edges.end(), unreleased),
                            Order.edges.end());
    State.node_states.erase(std::remove_if(State.node_states.begin(), State.node_states.end(), unreleased),
                             State.node_states.end());
    State.edge_states.erase(std::remove_if(State.edge_states.begin(), State.edge_states.end(), unreleased),
                             State.edge_states.end());
    State.action_states.erase(
        std::remove_if(State.action_states.begin(), State.action_states.end(),
                       [&](const auto& a) { return horizon_action_ids.count(a.action_id) > 0; }),
        State.action_states.end());
}

void RobotSimulator::append_states(const vda5050::Order& order, std::size_t first_node_index){
    using namespace vda5050;

    auto add_actions = [&](const std::vector<Action>& actions) {
        for (const auto& a : actions) {
            ActionState st;
            st.action_id = a.action_id;
            st.action_type = a.action_type;
            st.action_descriptor = a.action_descriptor;
            st.action_status = ActionStatus::Waiting;
            State.action_states.push_back(std::move(st));
        }
    };

    for (std::size_t i = first_node_index; i < order.nodes.size(); ++i) {
        const auto& node = order.nodes[i];
        Order.nodes.push_back(node);
        add_actions(node.actions);
        if (i == 0) {
            continue;
        }
        NodeState ns;
        ns.node_id = node.node_id;
        ns.sequence_id = node.sequence_id;
        ns.node_descriptor = node.node_descriptor;
        ns.released = node.released;
        ns.node_position = node.node_position;
        State.node_states.push_back(std::move(ns));
    }

    for (const auto& edge : order.edges) {
        Order.edges.push_back(edge);
        add_actions(edge.actions);
        EdgeState es;
        es.edge_id = edge.edge_id;
        es.sequence_id = edge.sequence_id;
        es.edge_descriptor = edge.edge_descriptor;
        es.released = edge.released;
        es.trajectory = edge.trajectory;
        State.edge_states.push_back(std::move(es));
    }
}

const vda5050::Node* RobotSimulator::decision_point() const{
    const auto it = std::find_if(Order.nodes.rbegin(), Order.nodes.rend(),
                                     [](const vda5050::Node& n) { return n.released; });
    return it == Order.nodes.rend() ? nullptr : &*it;
}


bool RobotSimulator::is_start_node_in_range(const vda5050::Node& node) const{
    // Без координат (line-guided) — робот должен стоять на этом же узле.
    if (!node.node_position) {
        return State.last_node_id.empty() || State.last_node_id == node.node_id;
    }
    if (!State.mobile_robot_position || !State.mobile_robot_position->localized) {
        return false;
    }
    const auto& pos = State.mobile_robot_position.value();
    const auto& np = node.node_position.value();
    if (pos.map_id != np.map_id) {
        return false;
    }

    const double dx = pos.x - np.x;
    const double dy = pos.y - np.y;

    if (!np.allowed_deviation_xy) {
        return std::hypot(dx, dy) <= default_start_tolerance_;
    }
    // Переводим смещение в систему координат эллипса.
    const auto& e = *np.allowed_deviation_xy;
    if (e.a <= 0.0 || e.b <= 0.0) {
        return std::hypot(dx, dy) <= 1e-3;
    }
    const double c = std::cos(e.theta), s = std::sin(e.theta);
    const double u = c * dx + s * dy;
    const double v = -s * dx + c * dy;
    return (u * u) / (e.a * e.a) + (v * v) / (e.b * e.b) <= 1.0;
}


void RobotSimulator::clear_order_errors(){
    State.errors.erase(std::remove_if(State.errors.begin(), State.errors.end(),
                                          [](const vda5050::Error& e) {
                                              return kOrderRejectionErrors.count(e.error_type) > 0;
                                          }),
                           State.errors.end());
}

std::optional<RobotSimulator::Rejection> RobotSimulator::validate_capabilities(const vda5050::Order& order) const{
    using namespace vda5050;

    auto check_actions = [&](const std::vector<Action>& actions) -> std::optional<Rejection> {
        if (supported_action_types_.empty()) {
            return std::nullopt;
        }
        for (const auto& a : actions) {
            if (!supported_action_types_.count(a.action_type)) {
                return Rejection{error_type::kInvalidOrderAction, ErrorLevel::Warning,
                                 "Unsupported action " + a.action_type,
                                 {error_ref("actionId", a.action_id), error_ref("actionType", a.action_type)}};
            }
        }
        return std::nullopt;
    };

    for (const auto& node : order.nodes) {
        if (auto r = check_actions(node.actions)) return r;

        if (node.node_position && State.maps) {
            const auto& maps = *State.maps;
            const bool enabled = std::any_of(maps.begin(), maps.end(), [&](const Map& m) {
                return m.map_id == node.node_position->map_id && m.map_status == EnableStatus::Enabled;
            });
            if (!enabled) {
                return Rejection{error_type::kUnknownMapId, ErrorLevel::Warning,
                                 "Unknown map " + node.node_position->map_id,
                                 {error_ref("nodeId", node.node_id)}};
            }
        }
    }
    for (const auto& edge : order.edges) {
        if (auto r = check_actions(edge.actions)) return r;
    }
    return std::nullopt;
}


std::optional<RobotSimulator::Rejection> RobotSimulator::validate_structure(const vda5050::Order& order) const{
    using namespace vda5050;
    auto fail = [](std::string why) {
        return Rejection{error_type::kValidationFailure, ErrorLevel::Warning, std::move(why), {}};
    };

    if (order.nodes.empty()) {
        return fail("Order has no nodes");
    }
    if (order.edges.size() != order.nodes.size() - 1) {
        return fail("Number of edges must be number of nodes minus one");
    }

    const std::uint32_t base_seq = order.nodes.front().sequence_id;
    if (base_seq % 2 != 0) {
        return fail("Node sequenceId must be even");
    }
    for (std::size_t i = 0; i < order.nodes.size(); ++i) {
        if (order.nodes[i].sequence_id != base_seq + 2 * i) {
            return fail("Node sequenceId is not continuous at nodeId " + order.nodes[i].node_id);
        }
    }
    for (std::size_t i = 0; i < order.edges.size(); ++i) {
        if (order.edges[i].sequence_id != base_seq + 2 * i + 1) {
            return fail("Edge sequenceId is not continuous at edgeId " + order.edges[i].edge_id);
        }
    }

    if (!order.nodes.front().released) {
        return fail("First node must be released");
    }
    // Base — непрерывный префикс: после первого unreleased released быть не может.
    // Ребро released только вместе с обоими узлами.
    bool horizon_started = false;
    for (std::size_t i = 0; i < order.edges.size(); ++i) {
        const auto& edge = order.edges[i];
        const auto& next = order.nodes[i + 1];
        if (edge.released && !next.released) {
            return fail("Released edge " + edge.edge_id + " leads to unreleased node");
        }
        if (horizon_started && (edge.released || next.released)) {
            return fail("Released element after horizon at edgeId " + edge.edge_id);
        }
        if (!edge.released) {
            horizon_started = true;
        }
    }
    return std::nullopt;
}

void RobotSimulator::reject(const Rejection& r, const vda5050::Order* order){
    using namespace vda5050;

    std::cerr << "[VDA 5050] Order rejected: " << r.type << " — " << r.description << '\n';

    Error err;
    err.error_type = r.type;
    err.error_level = r.level;
    err.error_description = r.description;
    auto refs = r.references;
    if (order && std::none_of(refs.begin(), refs.end(),
                              [](const auto& x) { return x.reference_key == "orderId"; })) {
        refs.push_back(error_ref("orderId", order->order_id));
                              }
    if (!refs.empty()) {
        err.error_references = std::move(refs);
    }

    State.errors.erase(std::remove_if(State.errors.begin(), State.errors.end(),
                                       [&](const Error& e) { return e.error_type == r.type; }),
                        State.errors.end());
    State.errors.push_back(std::move(err));
}


std::expected<vda5050::Order, std::string> RobotSimulator::parse_order(const std::string& json_order){
    namespace vda = vda5050;

    auto fail = [&](const std::string& why) -> std::unexpected<std::string> {
        reject({vda::error_type::kValidationFailure, vda::ErrorLevel::Warning, why, {}}, nullptr);
        return std::unexpected(why);
    };

    if (json_order.empty()) {
        return fail("Empty order message");
    }

    try {
        return vda::json::parse(json_order).get<vda::Order>();
    } catch (const vda::json::parse_error& e) {
        return fail(std::string("Order is not valid JSON: ") + e.what());
    } catch (const vda::json::out_of_range& e) {
        return fail(std::string("Order misses a mandatory field: ") + e.what());
    } catch (const vda::json::type_error& e) {
        return fail(std::string("Order field has wrong type: ") + e.what());
    } catch (const std::invalid_argument& e) {
        return fail(std::string("Order has unknown enum value: ") + e.what());
    } catch (const std::exception& e) {
        return fail(std::string("Malformed order: ") + e.what());
    }
}


bool same_content(const vda5050::Order& a, const vda5050::Order& b) {
    // header не сравниваем: при повторной отправке меняются headerId и timestamp.
    return vda5050::json(a.nodes) == vda5050::json(b.nodes) &&
           vda5050::json(a.edges) == vda5050::json(b.edges) &&
           a.order_description == b.order_description;
}

void RobotSimulator::update_order(const std::string& json_order) {
    namespace vda = vda5050;
    auto parsed = parse_order(json_order);
    if (!parsed) {
        std::println(stderr,"[RobotSim][{}] Failed to parse: {}", get_name(), parsed.error());
        return;
    }

    const vda::Order& incoming = parsed.value();

    if (auto r = validate_structure(incoming)) {
        reject(*r, &incoming);
        return;
    }

    if (State.operating_mode != vda::OperatingMode::Automatic &&
        State.operating_mode != vda::OperatingMode::Semiautomatic) {
        reject({vda::error_type::kMobileRobotNotAvailable, vda::ErrorLevel::Warning,
                "Operating mode does not allow orders", {}},
               &incoming);
        return;
    }

    if (auto r = validate_capabilities(incoming)) {
        reject(*r, &incoming);
        return;
    }

    const bool is_new_order = incoming.order_id != State.order_id;

    if (is_new_order) {
        if (!is_idle()) {
            reject({vda::error_type::kOtherOrderActive, vda::ErrorLevel::Warning,
                    "Another order is active: " + State.order_id,
                    {error_ref("orderId", incoming.order_id)}},
                   &incoming);
            return;
        }
        // (4) Новый order всегда начинается с orderUpdateId = 0.
        if (incoming.order_update_id != 0) {
            reject({vda::error_type::kValidationFailure, vda::ErrorLevel::Warning,
                    "New order must start with orderUpdateId 0",
                    {error_ref("orderUpdateId", std::to_string(incoming.order_update_id))}},
                   &incoming);
            return;
        }
        // (5) Первый узел тривиально достижим.
        if (!is_start_node_in_range(incoming.nodes.front())) {
            reject({vda::error_type::kStartNodeOutOfRange, vda::ErrorLevel::Warning,
                    "Start node is out of range",
                    {error_ref("nodeId", incoming.nodes.front().node_id)}},
                   &incoming);
            return;
        }

        // Удалить состояния прошлого order и принять новый.
        State.node_states.clear();
        State.edge_states.clear();
        State.action_states.clear();

        Order = vda::Order{};
        Order.order_id = incoming.order_id;
        current_segment_index = 0;
        order_canceled = false;

        State.order_id = incoming.order_id;
        State.order_update_id = incoming.order_update_id;
        // Первый узел не попадает в nodeStates (6.1.1): робот уже на нём.
        State.last_node_id = incoming.nodes.front().node_id;
        State.last_node_sequence_id = incoming.nodes.front().sequence_id;

        append_states(incoming, 0);

        std::cout << "[VDA 5050] New order " << incoming.order_id << " accepted\n";
    } else {
        // (6) Устаревший update.
        if (incoming.order_update_id < State.order_update_id) {
            reject({vda::error_type::kOutdatedOrderUpdate, vda::ErrorLevel::Warning,
                    "Order update is outdated",
                    {error_ref("orderId", incoming.order_id),
                     error_ref("orderUpdateId", std::to_string(incoming.order_update_id))}},
                   &incoming);
            return;
        }
        // (7) После cancelOrder update'ы к этому order запрещены.
        if (order_canceled) {
            reject({vda::error_type::kOrderUpdateFollowingCancel, vda::ErrorLevel::Warning,
                    "Order was cancelled", {error_ref("orderId", incoming.order_id)}},
                   &incoming);
            return;
        }
        // (8) Тот же orderUpdateId: повтор — молча игнорируем, другое содержимое — ошибка.
        if (incoming.order_update_id == State.order_update_id) {
            if (same_content(incoming, Last_received_order)) {
                std::println(stderr,"[RobotSim][{}] Error: resending the order", get_name());
                return;
            }
            reject({vda::error_type::kSameOrderUpdateId, vda::ErrorLevel::Warning,
                    "Same orderUpdateId with different content",
                    {error_ref("orderId", incoming.order_id),
                     error_ref("orderUpdateId", std::to_string(incoming.order_update_id))}},
                   &incoming);
            return;
        }

        // (9) / (10) Первый узел update'а — decision point (последний узел base).
        const vda::Node* dp = decision_point();
        const vda::Node& stitch = incoming.nodes.front();
        if (!dp || dp->node_id != stitch.node_id || dp->sequence_id != stitch.sequence_id) {
            reject({vda::error_type::kValidationFailure, vda::ErrorLevel::Warning,
                    "Order update does not start at the decision point",
                    {error_ref("nodeId", stitch.node_id),
                     error_ref("sequenceId", std::to_string(stitch.sequence_id))}},
                   &incoming);
            return;
        }

        // (9) Робот ещё едет или ждёт update: старый horizon заменяется новым.
        // (10) Робот idle: horizon и так пуст.
        drop_horizon();

        State.order_update_id = incoming.order_update_id;
        // Stitching node уже есть в order — добавляем всё после него.
        append_states(incoming, 1);

        std::cout << "[VDA 5050] Order " << incoming.order_id << " updated to "
                  << incoming.order_update_id << '\n';
    }

    Last_received_order = incoming;
    clear_order_errors();
    has_active_order_ = true;
}

std::vector<std::string> RobotSimulator::get_subscribe_topics(){
    std::vector<std::string> topics;
    topics.push_back(sub_order_topic);
    topics.push_back(sub_instantAction_topic);
    return topics;
}


void RobotSimulator::standstill(){
    drive_controller.standstill();
}

void RobotSimulator::charge_battery(){
}

void RobotSimulator::move_to(){
}



// void RobotSimulator::update_battery()
// {
//     if (robot_state.batteryState.charging) {
//         robot_state.batteryState.batteryCharge += CHARGE_RATE * 1;
//         if (robot_state.batteryState.batteryCharge >= 100.0) {
//             robot_state.batteryState.batteryCharge = 100.0;
//             robot_state.batteryState.batteryCharge = false;
//         }
//     }
//     else {
//         double current_rate = robot_state.driving ? DISCHARGE_RATE_DRIVE : DISCHARGE_RATE_IDLE;
//         robot_state.batteryState.batteryCharge -= current_rate * 1;
//         if (robot_state.batteryState.batteryCharge <= 0.0) {
//             robot_state.batteryState.batteryCharge = 0.0;
//         }
//     }
//
//     robot_state.batteryState.powerSavingMode = (robot_state.batteryState.batteryCharge < 15.0);
// }


// void RobotSimulator::update_order(std::string json_order){
//     if (json_order.empty()) {
//         std::cerr << "[RobotSimulator] Received empty JSON order string!" << std::endl;
//         return;
//     }
//     try {
//         nlohmann::json j = nlohmann::json::parse(json_order);
//
//         std::string incoming_order_id = j.at("orderId").get<std::string>();
//         uint32_t incoming_update_id = j.at("orderUpdateId").get<uint32_t>();
//
//         bool is_new_global_order = (incoming_order_id != current_order_id);
//         bool is_valid_update = (incoming_update_id > last_order_update_id);
//
//         if (!is_new_global_order && !is_valid_update) {
//             std::cout << "[VDA 5050] Ignored old or duplicate order message. "
//                       << "Current Update ID: " << last_order_update_id
//                       << ", Incoming Update ID: " << incoming_update_id << std::endl;
//             return;
//         }
//
//         OrderMessage incoming_order = j.get<OrderMessage>();
//
//         if (is_new_global_order) {
//             std::cout << "[VDA 5050] NEW GLOBAL ORDER RECEIVED! "
//                       << "Old ID: " << current_order_id << " -> New ID: " << incoming_order.orderId
//                       << " (Update ID: " << incoming_order.orderUpdateId << ")" << std::endl;
//
//             this->robot_order.nodes.clear();
//             this->robot_order.edges.clear();
//
//             this->current_segment_index = 0;
//             this->order_canceled = false;
//
//             current_order_id = incoming_order.orderId;
//
//             if (blackboard) {
//                 blackboard->set<bool>("has_active_order", true);
//             }
//         } else {
//             std::cout << "[VDA 5050] ORDER UPDATE RECEIVED for " << current_order_id
//                       << ". Appending new segments. Update ID: " << last_order_update_id
//                       << " -> " << incoming_order.orderUpdateId << std::endl;
//         }
//
//         last_order_update_id = incoming_order.orderUpdateId;
//         this->robot_order.nodes.insert(
//             this->robot_order.nodes.end(),
//             std::make_move_iterator(incoming_order.nodes.begin()),
//             std::make_move_iterator(incoming_order.nodes.end())
//         );
//
//         this->robot_order.edges.insert(
//             this->robot_order.edges.end(),
//             std::make_move_iterator(incoming_order.edges.begin()),
//             std::make_move_iterator(incoming_order.edges.end())
//         );
//
//         for (const auto& node : incoming_order.nodes) {
//             for (const auto& action : node.actions) {
//                 if (action.actionType == "cancelOrder") {
//                     this->order_canceled = true;
//                     std::cout << "[VDA 5050] Emergency cancelOrder action detected inside nodes!" << std::endl;
//                     break;
//                 }
//             }
//             if (this->order_canceled) break;
//         }
//
//         this->emit_state_changed();
//
//     } catch (const nlohmann::json::parse_error& e) {
//         std::cerr << "[RobotSimulator] JSON syntax parse error: " << e.what() << std::endl;
//     } catch (const nlohmann::json::type_error& e) {
//         std::cerr << "[RobotSimulator] JSON field type mismatch error: " << e.what() << std::endl;
//     } catch (const std::exception& e) {
//         std::cerr << "[RobotSimulator] Unknown error while processing order: " << e.what() << std::endl;
//     }
// }


// using namespace asio::experimental::awaitable_operators;
// constexpr auto no_throw = asio::as_tuple(asio::use_awaitable);
//
// while (!stopping_) {
//     // Ждём событие или max_state_interval_ — что раньше.
//     state_timer_.expires_after(max_state_interval_);
//     auto result = co_await (state_trigger_channel->async_receive(no_throw) ||
//                             state_timer_.async_wait(no_throw));
//
//     if (stopping_) {
//         break;
//     }
//
//     const bool by_trigger = result.index() == 0;
//     const boost::system::error_code ec =
//         by_trigger ? std::get<0>(std::get<0>(result)) : std::get<0>(std::get<1>(result));
//
//     if (ec) {
//         // Канал закрыт или таймер отменён не через stop() — продолжать нельзя.
//         std::cerr << "[State] wait failed: " << ec.message() << std::endl;
//         break;
//     }
//
//     // Триггер мог прийти на уже отправленные изменения — тогда слать нечего.
//     if (by_trigger && state_sent_once_ && state_ == last_sent_state_) {
//         continue;
//     }
//
//     try {
//         publish_state();
//     } catch (const std::exception& e) {
//         // Одна неудачная отправка не должна останавливать отправку state навсегда.
//         // last_sent_state_ не обновился, поэтому следующий триггер или таймер отправит снова.
//         std::cerr << "[State] publish failed: " << e.what() << std::endl;
//     }
//
//     // Не чаще minimumStateInterval: события за это время схлопнутся в одну отправку.
//     state_timer_.expires_after(min_state_interval_);
//     auto [wait_ec] = co_await state_timer_.async_wait(no_throw);
//     if (wait_ec && !stopping_) {
//         std::cerr << "[State] rate limit wait failed: " << wait_ec.message() << std::endl;
//         break;
//     }
// }
// std::cout << "[State] sender stopping" << std::endl;