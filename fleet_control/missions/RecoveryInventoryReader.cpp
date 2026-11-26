#include "missions/MissionRunner.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace dp = fms::dispatch::v1;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

const MissionRunner::Inventory* MissionRunner::current_inventory() const {
    return inventory_cache_.current(Clock::now());
}

void MissionRunner::wake_inventory_if_idle() {
    if (inventory_idle_) inventory_timer_.cancel_one();
}

void MissionRunner::invalidate_inventory() {
    ++assessment_source_epoch_;
    inventory_cache_.invalidate();
    inventory_timer_.cancel_one();
}

asio::awaitable<void> MissionRunner::inventory_loop() {
    bool previous_read_ok = false;
    while (!stopped_) {
        if (Clock::now() < inventory_next_read_) {
            inventory_timer_.expires_at(inventory_next_read_);
            co_await inventory_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
            continue;
        }
        const auto epoch = assessment_source_epoch_;
        const auto issued = Clock::now();
        inventory_next_read_ = issued + 1s;
        dp::GetRecoveryInventoryRequest request;
        Inventory reply;
        const auto status = co_await services_.call<&dp::FleetDispatch::Stub::PrepareAsyncGetRecoveryInventory>(
            *services_.dispatch, request, reply);
        if (stopped_) co_return;
        if (epoch != assessment_source_epoch_) continue;

        const auto& previous = inventory_cache_.latest();
        // Recheck each row against every new revision. Only a source or trust
        // transition invalidates checkpoint proofs for unrelated robots.
        const bool trust_changed = previous.instance_id() != reply.instance_id() ||
                                   previous.state() != reply.state();
        const bool timely = Clock::now() < issued + 3s;
        const bool accepted = status.ok() && timely && inventory_cache_.accept(std::move(reply), issued + 3s);
        if (!accepted) inventory_cache_.invalidate();
        if ((accepted && trust_changed) || (!accepted && previous_read_ok)) ++assessment_source_epoch_;
        previous_read_ok = accepted;

        inventory_idle_ = accepted && inventory_cache_.latest().state() == Inventory::READY &&
                          traffic_snapshot_ && checkpoint_jobs_.empty();
        if (inventory_idle_) inventory_timer_.expires_at(Clock::time_point::max());
        else inventory_timer_.expires_after(1s);
        co_await inventory_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        inventory_idle_ = false;
    }
}
