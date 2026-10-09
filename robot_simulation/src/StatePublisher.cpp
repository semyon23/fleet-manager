
#include "../include/StatePublisher.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include "spdlog/spdlog.h"
#include "../../shared/include/coro_guard.hpp"
StatePublisher:: StatePublisher(asio::any_io_executor ex, ptrPublishingChannel channel, vda::State& state, VdaTopics topics, PublisherConfig config)
    : channel_(channel), State_(state), topics_(topics), config_(config), state_trigger(ex, 1), visu_timer(ex), state_timer(ex)
{}

void StatePublisher::stop(){
    stopping_ = true;
    state_trigger.close();
    visu_timer.cancel();
    state_timer.cancel();
}

void StatePublisher::on_link_up(){
    this->pub_connection_topic();
    this->pub_factsheet_topic();
}

void StatePublisher::publish_factsheet(){
    this->pub_factsheet_topic();
}

void StatePublisher::set_factsheet(vda5050::Factsheet factsheet){
    Factsheet_ = std::move(factsheet);
}

asio::awaitable<void> StatePublisher::run_state_publisher_loop() {
    spdlog::info("[publisher][{}] state publisher loop start", State_.header.serial_number);
    CoroGuard guard([name = State_.header.serial_number] {
        spdlog::info("[publisher][{}] state publisher loop stopped", name);
    });

    while (!stopping_) {
        state_timer.expires_after(config_.max_state_interval);

        auto [order, err_timer, err_channel, state_received] =
            co_await asio::experimental::make_parallel_group(
                state_timer.async_wait(asio::deferred),
                state_trigger.async_receive(asio::deferred))
                .async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);

        if (stopping_) {
            co_return;
        }
        if (order[0] == 0 ? bool(err_timer) : bool(err_channel)) {
            co_return;
        }
        pub_state_topic();
        state_timer.expires_after(config_.min_state_interval);
        auto [ec] = co_await state_timer.async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) {
            co_return;
        }
    }
}

asio::awaitable<void> StatePublisher::run_visu_publisher_loop(){
    asio::steady_timer timer(co_await asio::this_coro::executor);
    spdlog::info("[publisher][{}] visu publisher loop start", State_.header.serial_number);
    CoroGuard guard([name = State_.header.serial_number] {
            spdlog::info("[publisher][{}] visu publisher loop stopped", name);
        });
    while (!stopping_) {
        visu_timer.expires_after(config_.visualization_interval);
        auto [ec] = co_await visu_timer.async_wait(asio::as_tuple(asio::use_awaitable));

        if (ec) {
            co_return;
        }
        this->pub_visu_topic();
    }
}

void StatePublisher::emit_state_changed(){
    if (state_sent_once && State_ == Last_sent_state) {
        return;
    }
    if (!state_trigger.is_open()) {
        spdlog::error("[publisher] Trigger channel is not ready or closed");
        return;
    }

    bool success = state_trigger.try_send(boost::system::error_code{}, true);

    if (!success) {
        spdlog::error("[publisher] Trigger did not activate (Buffer is full)");
    }
}

void StatePublisher::pub_connection_topic(){
    if (!channel_ || !channel_->is_open()){
        spdlog::error("[publisher] The channel is unavailable for publishing in topic :{}", topics_.connection);
        return;
    }
    Connection_.header = make_header(Connection_.header.header_id);
    Connection_.connection_state = vda::ConnectionState::Online;
    PublishMessage msg{topics_.connection, vda5050::json(Connection_).dump(), 1, true};
    auto success = channel_->try_send(boost::system::error_code{}, msg);
    spdlog::info("[publisher][{}] The message (Connection) has been published in the topic : {}",State_.header.serial_number, topics_.connection);
    if (!success){
        spdlog::error("[publisher] The message was not published in the topic : {}", topics_.connection);
    }
}

void StatePublisher::pub_state_topic(){
    if (!channel_ || !channel_->is_open()){
        spdlog::error("[publisher] The channel is unavailable for publishing in topic :{}", topics_.state);
        return;
    }

    State_.header = make_header(State_.header.header_id);
    PublishMessage msg{topics_.state, vda5050::json(State_).dump(), 0, false};

    Last_sent_state = State_;
    state_sent_once = true;
    auto success = channel_->try_send(boost::system::error_code{}, msg);
    if (!success){
        spdlog::error("[publisher] The message was not published in the topic : {}", topics_.connection);
    }
}

void StatePublisher::pub_visu_topic(){
    if (!channel_ || !channel_->is_open()){
        spdlog::error("[publisher] The channel is unavailable for publishing in topic :{}", topics_.visualization);
        return;
    }
    Visu_.header = make_header(Visu_.header.header_id);
    Visu_.reference_state_header_id = State_.header.header_id;
    Visu_.mobile_robot_position = State_.mobile_robot_position;
    Visu_.velocity = State_.velocity;
    Visu_.planned_path = State_.planned_path;
    Visu_.intermediate_path = State_.intermediate_path;
    PublishMessage msg{topics_.visualization, vda5050::json(Visu_).dump(), 0, false};
    auto success = channel_->try_send(boost::system::error_code{}, msg);
    if (!success){
        spdlog::error("[publisher] The message was not published in the topic : {}", topics_.visualization);
    }
}

void StatePublisher::pub_factsheet_topic(){
    if (!channel_ || !channel_->is_open()){
        spdlog::error("[publisher] The channel is unavailable for publishing in topic :{}", topics_.factsheet);
        return;
    }
    Factsheet_.header = make_header(Factsheet_.header.header_id);
    PublishMessage msg{topics_.factsheet, vda5050::json(Factsheet_).dump(), 0, true};
    auto success = channel_->try_send(boost::system::error_code{}, msg);
    if (!success){
        spdlog::error("[publisher] The message was not published in the topic : {}", topics_.factsheet);
    }
}

vda::Header StatePublisher::make_header(std::uint32_t& counter) const {
    vda::Header h = State_.header;
    h.header_id = ++counter;
    h.timestamp = vda5050::make_timestamp();
    return h;
}