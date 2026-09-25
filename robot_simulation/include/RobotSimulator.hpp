#ifndef FLEETMANAGERSYSTEM_ROBOTSIMULATOR_HPP
#define FLEETMANAGERSYSTEM_ROBOTSIMULATOR_HPP
#include <string>
#include <nlohmann/json.hpp>
#include "../vda5050/state/State.hpp"
#include "../vda5050/order/Order.hpp"
#include "../vda5050/connection/Connection.hpp"
#include "../vda5050/visualization/Visualization.hpp"
#include "include/DiffDriveController.hpp"
#include "behaviortree_cpp/bt_factory.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/co_spawn.hpp>

#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/this_coro.hpp>
#include <chrono>
#include <print>

namespace asio = boost::asio;

struct RobotMessage{
    std::string topic;
    std::string payload;
};

struct RobotConfig{
    std::string name;
    std::string manufacturer;
    std::string map;
};

using publishing_channel = asio::experimental::channel<void(boost::system::error_code, RobotMessage)>;
using Pub_channel_ptr = std::shared_ptr<publishing_channel>;

class RobotSimulator : public std::enable_shared_from_this<RobotSimulator>{
public:
    RobotSimulator(RobotConfig conf, Pub_channel_ptr channel);
    void start(asio::any_io_executor executor);
    void stop();
    void init_behavior_tree(BT::BehaviorTreeFactory& factory, const std::string& xml_path);

    void update_order(std::string json_order);
    std::vector<std::string> get_subscribe_topics();

    void emit_state_changed();

    void standstill();
    void charge_battery();
    void execute_action();
    void start_action();
    void move_to();

    std::string get_name() const;
private:
    void update_battery();
    void init_state();

    RobotMessage pub_state_topic();
    RobotMessage pub_visu_topic();
    RobotMessage pub_connection_topic();

    asio::awaitable<void> run_simulation_loop();
    asio::awaitable<void> run_visu_publisher_loop();
    asio::awaitable<void> run_state_publisher_loop();

    RobotConfig config;
    State robot_state;
    OrderMessage robot_order;
    ConnectionMessage robot_connection;
    VisualizationMessage robot_visualization;

    std::string sub_order_topic;
    std::string sub_instantAction_topic;

    std::string state_topic;
    std::string visu_topic;
    std::string connection_topic;

    DiffDriveController robot_controller;

    const double DISCHARGE_RATE_IDLE{0.01};
    const double DISCHARGE_RATE_DRIVE{0.05};
    const double CHARGE_RATE{0.2};

    BT::Tree tree;
    BT::Blackboard::Ptr blackboard;

    Pub_channel_ptr pub_channel;
    std::unique_ptr<asio::experimental::channel<void(boost::system::error_code, bool)>> state_trigger_channel;
    std::unique_ptr<asio::steady_timer> simulation_timer;

    std::string current_order_id{""};     // Текущий активный orderId
    uint32_t last_order_update_id{0};     // Последний обработанный orderUpdateId
    size_t current_segment_index{0};      // Индекс сегмента, который сейчас обрабатывает дерево
    bool order_canceled{false};           // Флаг экстренной отмены (cancelOrder)
};


#endif //FLEETMANAGERSYSTEM_ROBOTSIMULATOR_HPP
