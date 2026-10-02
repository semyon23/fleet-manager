#ifndef FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
#define FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
#include <memory>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel.hpp>
namespace asio = boost::asio;

class RoboticVehicleSimulator : public std::enable_shared_from_this<RoboticVehicleSimulator>{
public:
    RoboticVehicleSimulator();
    void start(asio::any_io_executor executor);
    void stop();

    void handle_order(const std::string& json);
    void handle_instant_action(const std::string& json);
    void on_link_up();
};


#endif //FLEETMANAGERSYSTEM_ROBOTICVEHICLESIMULATOR_HPP
