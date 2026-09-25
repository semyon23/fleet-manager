//#include <utility>

#include "../include/RobotSimulator.hpp"
#include <chrono>
#include <format>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
RobotSimulator::RobotSimulator(RobotConfig conf, Pub_channel_ptr channel)
    : config(conf),
    pub_channel(std::move(channel)),
    robot_controller(1.0,1.0,0.0,0.1)
{
    this->init_state();
};

void RobotSimulator::init_behavior_tree(BT::BehaviorTreeFactory& factory, const std::string& xml_path)
{
    blackboard = BT::Blackboard::create();
    blackboard->set<std::shared_ptr<RobotSimulator>>("robot_instance", shared_from_this());
    tree = factory.createTreeFromFile(xml_path, blackboard);
}

void RobotSimulator::start(asio::any_io_executor executor)
{
    simulation_timer = std::make_unique<asio::steady_timer>(executor);
    state_trigger_channel = std::make_unique<asio::experimental::channel<void(boost::system::error_code, bool)>>(executor, 1);
    auto self = shared_from_this();
    boost::asio::co_spawn(executor, [self]() { return self->run_simulation_loop(); }, boost::asio::detached);
    boost::asio::co_spawn(executor, [self]() { return self->run_visu_publisher_loop(); }, boost::asio::detached);
    boost::asio::co_spawn(executor, [self]() { return self->run_state_publisher_loop(); }, boost::asio::detached);
}

void RobotSimulator::stop()
{
    simulation_timer->cancel();
    state_trigger_channel->cancel();
}

void RobotSimulator::emit_state_changed(){

    if (!state_trigger_channel || !state_trigger_channel->is_open()) {
        std::cerr << "Channel is not ready or closed." << std::endl;
        return;
    }

    bool success = state_trigger_channel->try_send(boost::system::error_code{}, true);

    if (success) {
        std::cout << "Trigger activate." << std::endl; // Лучше в std::cout, это не ошибка
    } else {
        // Сработает, только если буфер канала переполнен (никто не успел прочитать)
        std::cerr << "The trigger did not activate (Buffer is full)." << std::endl;
    }
}

asio::awaitable<void> RobotSimulator::run_simulation_loop()
{
    while (true) {
        simulation_timer->expires_after(std::chrono::milliseconds(200));
        auto [ec] = co_await simulation_timer->async_wait(asio::as_tuple(asio::use_awaitable));

        if (ec) {
            co_return;
        }
        tree.tickOnce();
        this->robot_controller.update_position();
    }
}

asio::awaitable<void> RobotSimulator::run_visu_publisher_loop()
{
    asio::steady_timer timer(co_await asio::this_coro::executor);
    while (true) {
        timer.expires_after(std::chrono::milliseconds(100000));
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
            std::cerr << "[Channel] Buffer is full or channel is closed. Message dropped!" << std::endl;
        }
    }
}

asio::awaitable<void> RobotSimulator::run_state_publisher_loop()
{
    auto executor = co_await boost::asio::this_coro::executor;
    asio::steady_timer periodic_timer{executor};

    while (true) {
        periodic_timer.expires_after(std::chrono::milliseconds(10000));

        auto [order, err_timer, err_channel, state_received] = co_await boost::asio::experimental::make_parallel_group(
            periodic_timer.async_wait(boost::asio::deferred),
            state_trigger_channel->async_receive(boost::asio::deferred)
        ).async_wait(boost::asio::experimental::wait_for_one(), asio::use_awaitable);

        size_t first_completed = order[0];

        if (first_completed == 0) {
            if (err_timer && err_timer != boost::asio::error::operation_aborted) {
                co_return;
            }
        }
        else if (first_completed == 1) {
            if (err_channel) {
                if (err_channel != boost::asio::error::operation_aborted) {
                    co_return;
                }
            }

            if (!state_received) {
                co_return;
            }

        }

        if (!pub_channel || !pub_channel->is_open()) {
            co_return;
        }

        auto state_packet = this->pub_state_topic();

        bool sent = pub_channel->try_send(boost::system::error_code{}, state_packet);
        if (!sent) {
            co_return;
        }
    }
}

std::string RobotSimulator::get_name() const
{
    return config.name;
};

RobotMessage RobotSimulator::pub_connection_topic()
{
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    robot_connection.timestamp = std::format("{:%FT%T}Z", now_ms);
    robot_connection.headerId++;
    nlohmann::json json_object = robot_connection;

    return {connection_topic, json_object.dump(5)};
}


RobotMessage RobotSimulator::pub_visu_topic()
{
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    robot_visualization.timestamp = std::format("{:%FT%T}Z", now_ms);
    robot_visualization.headerId++;
    nlohmann::json json_object = robot_visualization;

    return {visu_topic, json_object.dump(5)};
}

RobotMessage RobotSimulator::pub_state_topic()
{
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    robot_state.timestamp = std::format("{:%FT%T}Z", now_ms);
    robot_state.headerId++;
    robot_visualization.referenceStateHeaderId = robot_state.headerId;
    nlohmann::json json_object = robot_state;

    return {state_topic, json_object.dump(5)};
};

void RobotSimulator::update_order(std::string json_order){
    if (json_order.empty()) {
        std::cerr << "[RobotSimulator] Received empty JSON order string!" << std::endl;
        return;
    }
    try {
        // nlohmann::json json = nlohmann::json::parse(json_order);
        // OrderMessage incoming_order  = json.get<OrderMessage>();

       // 1. Легкий парсинг JSON без создания тяжелых C++ структур
        nlohmann::json j = nlohmann::json::parse(json_order);

        // Быстро вытаскиваем примитивные типы для проверки
        std::string incoming_order_id = j.at("orderId").get<std::string>();
        uint32_t incoming_update_id = j.at("orderUpdateId").get<uint32_t>();

        bool is_new_global_order = (incoming_order_id != current_order_id);
        bool is_valid_update = (incoming_update_id > last_order_update_id);

        // ОПТИМИЗАЦИЯ: Если это дубликат, мгновенно выходим БЕЗ маппинга в структуры
        if (!is_new_global_order && !is_valid_update) {
            std::cout << "[VDA 5050] Ignored old or duplicate order message. "
                      << "Current Update ID: " << last_order_update_id
                      << ", Incoming Update ID: " << incoming_update_id << std::endl;
            return;
        }

        OrderMessage incoming_order = j.get<OrderMessage>();

        if (is_new_global_order) {
            std::cout << "[VDA 5050] NEW GLOBAL ORDER RECEIVED! "
                      << "Old ID: " << current_order_id << " -> New ID: " << incoming_order.orderId
                      << " (Update ID: " << incoming_order.orderUpdateId << ")" << std::endl;

            this->robot_order.nodes.clear();
            this->robot_order.edges.clear();

            this->current_segment_index = 0;
            this->order_canceled = false;

            current_order_id = incoming_order.orderId;

            if (blackboard) {
                blackboard->set<bool>("has_active_order", true);
            }
        } else {
            std::cout << "[VDA 5050] ORDER UPDATE RECEIVED for " << current_order_id
                      << ". Appending new segments. Update ID: " << last_order_update_id
                      << " -> " << incoming_order.orderUpdateId << std::endl;
        }

        last_order_update_id = incoming_order.orderUpdateId;

        // ОПТИМИЗАЦИЯ: Эффективно переносим (std::move) новые узлы и ребра в конец векторов
        this->robot_order.nodes.insert(
            this->robot_order.nodes.end(),
            std::make_move_iterator(incoming_order.nodes.begin()),
            std::make_move_iterator(incoming_order.nodes.end())
        );

        this->robot_order.edges.insert(
            this->robot_order.edges.end(),
            std::make_move_iterator(incoming_order.edges.begin()),
            std::make_move_iterator(incoming_order.edges.end())
        );

        // Проверяем экстренную отмену (cancelOrder) по всем только что добавленным узлам
        for (const auto& node : incoming_order.nodes) {
            for (const auto& action : node.actions) {
                if (action.actionType == "cancelOrder") {
                    this->order_canceled = true;
                    std::cout << "[VDA 5050] Emergency cancelOrder action detected inside nodes!" << std::endl;
                    break;
                }
            }
            if (this->order_canceled) break;
        }

        this->emit_state_changed();

    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[RobotSimulator] JSON syntax parse error: " << e.what() << std::endl;
    } catch (const nlohmann::json::type_error& e) {
        std::cerr << "[RobotSimulator] JSON field type mismatch error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[RobotSimulator] Unknown error while processing order: " << e.what() << std::endl;
    }
}

std::vector<std::string> RobotSimulator::get_subscribe_topics(){
    std::vector<std::string> topics;
    topics.push_back(sub_order_topic);
    topics.push_back(sub_instantAction_topic);
    return topics;
}

void RobotSimulator::init_state()
{
    robot_state.serialNumber = config.name;
    robot_connection.serialNumber = config.name;
    robot_visualization.serialNumber = config.name;

    robot_state.manufacturer = config.manufacturer;
    robot_connection.manufacturer = config.manufacturer;
    robot_visualization.manufacturer = config.manufacturer;

    std::string base_topic = std::format("amr/v3/{}/{}/{}/",
    config.manufacturer, config.map, config.name);

    state_topic             = base_topic + "state";
    visu_topic              = base_topic + "visualization";
    connection_topic        = base_topic + "connection";

    sub_order_topic         = base_topic + "order";
    sub_instantAction_topic = base_topic + "instantAction";


    robot_state.operatingMode = OperatingMode::AUTOMATIC;
    robot_state.batteryState.batteryCharge = 100.0;
    robot_state.batteryState.batteryTemperature = 40.0;
    robot_state.batteryState.batteryVoltage = 60.0;
}

void RobotSimulator::update_battery()
{
    if (robot_state.batteryState.charging) {
        robot_state.batteryState.batteryCharge += CHARGE_RATE * 1;
        if (robot_state.batteryState.batteryCharge >= 100.0) {
            robot_state.batteryState.batteryCharge = 100.0;
            robot_state.batteryState.batteryCharge = false;
        }
    }
    else {
        double current_rate = robot_state.driving ? DISCHARGE_RATE_DRIVE : DISCHARGE_RATE_IDLE;
        robot_state.batteryState.batteryCharge -= current_rate * 1;
        if (robot_state.batteryState.batteryCharge <= 0.0) {
            robot_state.batteryState.batteryCharge = 0.0;
        }
    }

    robot_state.batteryState.powerSavingMode = (robot_state.batteryState.batteryCharge < 15.0);
}


void RobotSimulator::standstill()
{
    robot_state.velocity.vx = 0.0;
    robot_state.velocity.vy = 0.0;
    robot_state.velocity.omega = 0.0;

    robot_state.driving = false;
    robot_state.idle = true;

    robot_state.edgeStates.clear();
    robot_state.nodeStates.clear();
    robot_state.plannedPath.clear();
    robot_state.actionStates.clear();

    robot_state.blockingType = BlockingType::NONE;
    this->update_battery();
}

void RobotSimulator::charge_battery()
{
    robot_state.batteryState.batteryCharge = true;
    this->update_battery();
}

void RobotSimulator::move_to(){

}
