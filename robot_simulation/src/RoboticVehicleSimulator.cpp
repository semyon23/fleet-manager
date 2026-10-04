//
// Created by ubuntu on 10/1/26.
//

#include "../include/RoboticVehicleSimulator.hpp"
#include "../include/factsheet_builder.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/as_tuple.hpp>
#include "spdlog/spdlog.h"
RoboticVehicleSimulator::RoboticVehicleSimulator(asio::any_io_executor ex, RobotConfig config, ptrPublishingChannel channel)
    : executor(ex),
    config_(config),
    publisher_(ex, channel, State_, topics_, config.publisher),
    driver_(config.motion),
    simulation_timer_(executor){

    //publisher_.set_factsheet(build_factsheet(config_, actions_.supported_types()));
    //driver_.set_on_node_reached()
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
}

void RoboticVehicleSimulator::stop(){
    simulation_timer_.cancel();
    publisher_.stop();
}


void RoboticVehicleSimulator::on_link_up(){
    publisher_.on_link_up();
}

asio::awaitable<void> RoboticVehicleSimulator::run_simulation_loop(){
    using clock = std::chrono::steady_clock;
    constexpr auto period = std::chrono::milliseconds(config_.simulation.tick_period);

    auto next = clock::now();
    auto last = next;
    // spdlog::info("[{}] Run simulation loop start", get_name());
    // CoroGuard guard([this](){
    //     tree.haltTree();
    //     spdlog::info("[{}] Run simulation loop stopped", get_name());
    // });
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


        // has_active_order_ = !vda5050::is_idle(State);
        //tree.tickOnce();
        simulation_tick(dt);
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
}
