#ifndef FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
#define FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
#include <memory>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel.hpp>
#include "../vda5050/vda5050.hpp"
#include "include/StatePublisher.hpp"
#include "include/RobotDriveController.hpp"
namespace asio = boost::asio;


class RoboticVehicleSimulator :  public std::enable_shared_from_this<RoboticVehicleSimulator>{
public:
    RoboticVehicleSimulator(asio::any_io_executor ex, RobotConfig config, ptrPublishingChannel channel);
    void start();
    void stop();



    void handle_order(const std::string& json);
    void handle_instant_action(const std::string& json);
    void on_link_up();

private:
    asio::awaitable<void> run_simulation_loop();
    void simulation_tick(double dt);
    asio::any_io_executor executor;
    RobotConfig config_;
    VdaTopics topics_{config_.identity};

    vda5050::State State_;
    vda5050::Order Order_;

    StatePublisher publisher_;
    RobotDriveController driver_;;
    asio::steady_timer simulation_timer_;
    bool stopping_ = false;
};


#endif //FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
