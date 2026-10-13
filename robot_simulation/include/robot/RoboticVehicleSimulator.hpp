#ifndef FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
#define FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
#include <memory>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel.hpp>
#include "vda5050.hpp"
#include "behaviortree_cpp/bt_factory.h"
#include "include/vda/StatePublisher.hpp"
#include "include/vda/OrderManager.hpp"
#include "include/motion/RobotDriveController.hpp"
#include "include/behavior/action_nodes.hpp"
#include "include/actions/ActionExecutor.hpp"
#include "include/vda/ActionManager.hpp"
#include "include/actions/PickDropActions.hpp"

namespace asio = boost::asio;

struct TopicHandler {
    std::string topic;
    std::function<void(const std::string&)> handler;
};

class RoboticVehicleSimulator : public std::enable_shared_from_this<RoboticVehicleSimulator>,
                                public RobotBehaviorApi{
public:
    RoboticVehicleSimulator(asio::any_io_executor ex, RobotConfig config, ptrPublishingChannel channel);
    ~RoboticVehicleSimulator();
    void start();
    void stop();
    void init_behavior_tree(BT::BehaviorTreeFactory& factory);


    std::vector<TopicHandler> get_topic_routes();
    void on_link_up();

private:
    void handle_order(const std::string& json);
    void handle_instant_action(const std::string& json);

    void init_state();
    asio::awaitable<void> run_simulation_loop();
    void simulation_tick(double dt);
    void on_node_reached(const std::string& node_id);
    const vda5050::Node* find_next_node(const std::string& node_id) const;
    const vda5050::Edge* find_edge(std::uint32_t sequence_id);
    //void finish_edge_actions(const vda5050::Edge& edge);
    //vda5050::ActionState* find_action_state(const std::string& action_id);
    //void trigger_actions(const std::vector<vda5050::Action>& actions);
    const vda5050::Node* current_node() const;
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
    ActionManager action_manager_;


    BT::Tree tree_;

    asio::steady_timer simulation_timer_;
    bool stopping_ = false;

    bool segment_ends_blocking_{false};
    uint32_t segment_end_seq_{0};
};


#endif //FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
