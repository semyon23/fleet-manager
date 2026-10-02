#ifndef FLEETMANAGERSYSTEM_ROBOTSIMULATOR_HPP
#define FLEETMANAGERSYSTEM_ROBOTSIMULATOR_HPP
#include <string>
#include <nlohmann/json.hpp>
#include "include/RobotDriveController.hpp"
#include "../vda5050/vda5050.hpp"

#include "behaviortree_cpp/bt_factory.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>


#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/this_coro.hpp>
#include <chrono>
#include <print>
#include "behaviortree_cpp/loggers/groot2_publisher.h"
namespace asio = boost::asio;

struct RobotMessage{
    std::string topic;
    std::string payload;
};

struct RobotConfig{
    std::string name;
    std::string manufacturer;
    std::string map;
    DiffDriveParams drive_params;

    std::chrono::milliseconds min_state_interval_{100};
    std::chrono::seconds max_state_interval_{10};
};

using publishing_channel = asio::experimental::channel<void(boost::system::error_code, RobotMessage)>;
using Pub_channel_ptr = std::shared_ptr<publishing_channel>;

class RobotSimulator : public std::enable_shared_from_this<RobotSimulator> {
public:
    RobotSimulator(RobotConfig conf, Pub_channel_ptr channel);
    ~RobotSimulator();
    void start(asio::any_io_executor executor);
    void stop();
    void init_behavior_tree(BT::BehaviorTreeFactory& factory, const std::string& xml_path);

    void update_order(const std::string& json_order);
    void on_link_up();

    std::vector<std::string> get_subscribe_topics();

    bool has_active_order() const;


    void charge_battery();
    void execute_action();
    void start_action();
    void move_to();

    std::vector<DriveWaypoint> build_segment(std::uint32_t after_seq);
    bool path_finished() const;
    std::string get_name() const;
    vda5050::State& get_state();
    vda5050::Order get_last_received_order() const;
    void set_new_path(std::vector<DriveWaypoint> wps);
    void append_path(std::vector<DriveWaypoint> wps);
    bool start_new_segment();
    bool extend_current_segment();
    bool is_moving() const;
    void standstill();

private:
    void simulation_tick(double dt);
    struct Rejection {
        std::string type;
        vda5050::ErrorLevel level = vda5050::ErrorLevel::Warning;
        std::string description;
        std::vector<vda5050::ErrorReference> references;
    };

    bool is_blocking(const vda5050::Node& node);
    //std::optional<vda5050::Order> parse_order(const std::string& json_order);
    std::expected<vda5050::Order, std::string> parse_order(const std::string& json_order);
    std::optional<Rejection> validate_structure(const vda5050::Order& order) const;
    std::optional<Rejection> validate_capabilities(const vda5050::Order& order) const;
    void init_state();
    bool is_idle() const;
    bool is_start_node_in_range(const vda5050::Node& node) const;
    const vda5050::Node* decision_point() const;
    void drop_horizon();
    void append_states(const vda5050::Order& order, std::size_t first_node_index);
    void reject(const Rejection& r, const vda5050::Order* order);
    void clear_order_errors();

    void on_node_reached(const std::string& node_id);
    const vda5050::Edge* find_edge(std::uint32_t sequence_id);
    const vda5050::Node* find_next_node(const std::string& node_id) const;
    void trigger_actions(const std::vector<vda5050::Action>& actions);
    vda5050::ActionState* find_action_state(const std::string& action_id);
    void finish_edge_actions(const vda5050::Edge& edge);
    void emit_state_changed();

    RobotMessage pub_state_topic();
    RobotMessage pub_visu_topic();
    RobotMessage pub_connection_topic();

    asio::awaitable<void> run_simulation_loop();
    asio::awaitable<void> run_visu_publisher_loop();
    asio::awaitable<void> run_state_publisher_loop();

    RobotConfig config;
    RobotDriveController drive_controller;
    BT::Tree tree;
    BT::Blackboard::Ptr blackboard;

    // std::unique_ptr<BT::Groot2Publisher> groot;
    // unsigned groot_port = 1667;

    std::string sub_order_topic;
    std::string sub_instantAction_topic;

    std::string state_topic;
    std::string visu_topic;
    std::string connection_topic;
    std::string factsheet_topic;

    Pub_channel_ptr pub_channel;
    std::unique_ptr<asio::experimental::channel<void(boost::system::error_code, bool)>> state_trigger_channel;
    std::unique_ptr<asio::steady_timer> simulation_timer;

    vda5050::State State;
    vda5050::Order Order;
    vda5050::Connection Connection;
    vda5050::Visualization Visualization;

    vda5050::Order Last_received_order;
    vda5050::State Last_sent_state;
    bool state_sent_once = false;
    std::uint32_t state_header_id_ = 0;
    std::chrono::milliseconds min_state_interval_{100};    // factsheet: protocolLimits.timing.minimumStateInterval
    std::chrono::seconds max_state_interval_{30};

    double default_start_tolerance_{0.5};
    std::set<std::string> supported_action_types_;

    bool segment_ends_blocking_{false};
    uint32_t segment_end_seq_{0};

    std::atomic<bool> has_active_order_{false};
    bool stopping{false};

    std::string current_order_id{""};     // Текущий активный orderId
    uint32_t last_order_update_id{0};     // Последний обработанный orderUpdateId
    size_t current_segment_index{0};      // Индекс сегмента, который сейчас обрабатывает дерево
    bool order_canceled{false};           // Флаг экстренной отмены (cancelOrder)
};


#endif //FLEETMANAGERSYSTEM_ROBOTSIMULATOR_HPP
