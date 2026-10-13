
#include "../../include/actions/ChargingActions.hpp"
#include <memory>

namespace {

    class StartCharging : public RunningAction {
    public:
        StartCharging(vda5050::State& state, double start_time) : state_(state), remaining_(start_time) {}

        vda5050::ActionStatus update(double dt) override {
            if (state_.driving) {
                reason_ = "robot is driving";
                return vda5050::ActionStatus::Failed;
            }
            if (state_.power_supply.charging) {
                return vda5050::ActionStatus::Finished;   // уже заряжается
            }
            remaining_ -= dt;
            if (remaining_ > 0.0) {
                return vda5050::ActionStatus::Running;
            }
            state_.power_supply.charging = true;
            return vda5050::ActionStatus::Finished;
        }

        bool cancel() override { return false; }   // factsheet: cancelAllowed = false
        std::optional<std::string> failure_reason() const override { return reason_; }

    private:
        vda5050::State& state_;
        double remaining_;
        std::optional<std::string> reason_;
    };

    class StopCharging : public RunningAction {
    public:
        explicit StopCharging(vda5050::State& state) : state_(state) {}

        vda5050::ActionStatus update(double) override {
            state_.power_supply.charging = false;
            return vda5050::ActionStatus::Finished;
        }

    private:
        vda5050::State& state_;
    };

}  // namespace

void register_charging_actions(ActionExecutor& executor, const BatteryConfig& battery) {
    const double start_time = battery.charging_start_time;
    executor.register_handler(
        vda5050::action_type::kStartCharging,
        [start_time](const vda5050::Action&, vda5050::State& state) -> std::unique_ptr<RunningAction> {
            return std::make_unique<StartCharging>(state, start_time);
        });
    executor.register_handler(
        vda5050::action_type::kStopCharging,
        [](const vda5050::Action&, vda5050::State& state) -> std::unique_ptr<RunningAction> {
            return std::make_unique<StopCharging>(state);
        });
}