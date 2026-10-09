#ifndef FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
#define FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
#include <memory>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel.hpp>
#include "../vda5050/vda5050.hpp"
#include "behaviortree_cpp/bt_factory.h"
#include "include/StatePublisher.hpp"
#include "include/OrderManager.hpp"
#include "include/RobotDriveController.hpp"
#include "action_nodes.hpp"
#include "include/ActionExecutor.hpp"


namespace asio = boost::asio;

struct TopicHandler {
    std::string topic;
    std::function<void(const std::string&)> handler;
};

class RoboticVehicleSimulator : public std::enable_shared_from_this<RoboticVehicleSimulator>,
                                public RobotBehaviorApi{
public:
    RoboticVehicleSimulator(asio::any_io_executor ex, RobotConfig config, ptrPublishingChannel channel);
    void start();
    void stop();
    void init_behavior_tree(BT::BehaviorTreeFactory& factory);


    std::vector<TopicHandler> get_topic_routes();
    void on_link_up();

    // --- RobotBehaviorApi ---
private:
    void handle_order(const std::string& json);
    void handle_instant_action(const std::string& json);

    void init_state();
    asio::awaitable<void> run_simulation_loop();
    void simulation_tick(double dt);
    void on_node_reached(const std::string& node_id);
    const vda5050::Node* find_next_node(const std::string& node_id) const;
    const vda5050::Edge* find_edge(std::uint32_t sequence_id);
    void finish_edge_actions(const vda5050::Edge& edge);
    vda5050::ActionState* find_action_state(const std::string& action_id);
    void trigger_actions(const std::vector<vda5050::Action>& actions);

    std::vector<DriveWaypoint> build_segment(std::uint32_t after_seq);
    bool is_blocking(const vda5050::Node& node);


    std::string name() const override;
    bool is_safe() const override;
    bool is_automatic_mode() const override;
    bool is_paused() const override;
    bool has_active_order() const override;
    bool has_base_ahead() const override;
    bool at_decision_point() const override;
    bool start_segment() override;
    bool extend_segment() override;
    DriveStatus drive_status() const override;
    void stop_driving() override;
    void start_node_actions() override;
    bool node_actions_done() const override;
    void cancel_node_actions() override;
    void set_new_base_request(bool value) override;



    asio::any_io_executor executor;
    RobotConfig config_;
    VdaTopics topics_{config_.identity};

    vda5050::State State_;
    vda5050::Order Order_;
    vda5050::Order last_received_order_;
    bool order_canceled_ = false;

    StatePublisher publisher_;
    OrderManager order_manager_;
    RobotDriveController driver_;
    ActionExecutor action_executor_;

    BT::Tree tree_;

    asio::steady_timer simulation_timer_;
    bool stopping_ = false;

    bool segment_ends_blocking_{false};
    uint32_t segment_end_seq_{0};
};


#endif //FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP

// class RoboticVehicleSimulator : public std::enable_shared_from_this<RoboticVehicleSimulator>,
//                                 public RobotBehaviorApi {
// public:
//     RoboticVehicleSimulator(asio::any_io_executor ex, RobotConfig config, ptrPublishingChannel channel);
//
//     // --- жизненный цикл ---
//     void start();                                    // корутины: симуляция, state, visualization
//     void stop();
//     void shutdown();                                 // stop + OFFLINE
//
//     // --- для транспорта (вызывать через asio::post(executor(), ...)) ---
//     asio::any_io_executor executor() const { return ex_; }
//     const std::string& name() const { return config_.name(); }
//     std::vector<std::string> get_subscribe_topics() const { return topics_.subscriptions(); }
//     RobotMessage last_will() const { return publisher_.make_last_will(); }
//     void handle_order(const std::string& json);
//     void handle_instant_actions(const std::string& json);
//     void on_link_up();
//
//     // --- RobotBehaviorApi (для дерева) ---
//     bool is_safe() const override;
//     bool is_automatic_mode() const override;
//     bool is_paused() const override;
//     bool has_active_order() const override;
//     bool has_base_ahead() const override;
//     bool at_decision_point() const override;
//     bool start_segment() override;
//     bool extend_segment() override;
//     DriveStatus drive_status() const override;
//     void stop_driving() override;
//     void start_node_actions() override;
//     bool node_actions_done() const override;
//     void cancel_node_actions() override;
//     void set_new_base_request(bool value) override;
//
// private:
//     void init_state();
//     void init_behavior_tree();
//     asio::awaitable<void> run_simulation_loop();
//     void simulation_tick(double dt);
//     void on_node_reached(const std::string& node_id);
//
//     // instant action'ы
//     void cancel_order(const vda5050::Action& action);
//     void set_paused(bool paused);
//
//     // --- поля: порядок важен ---
//     asio::any_io_executor ex_;                       // первым: из него создаётся асинхронное
//     RobotConfig config_;
//     VdaTopics topics_{config_.identity};
//
//     vda5050::State State_;
//     vda5050::Order Order_;
//     vda5050::Order last_received_order_;
//     bool order_canceled_ = false;
//
//     ActionExecutor actions_{State_};                 // после State_
//     RobotDriveController driver_;                    // из config_.motion
//     StatePublisher publisher_;
//
//     BT::BehaviorTreeFactory bt_factory_;
//     BT::Tree tree_;
//     std::unique_ptr<BT::Groot2Publisher> groot_;     // после tree_: удаляется раньше дерева
//
//     asio::steady_timer simulation_timer_{ex_};
//     bool stopping_ = false;
// };