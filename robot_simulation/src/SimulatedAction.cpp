
#include "../include/SimulatedAction.hpp"
#include <algorithm>
#include <memory>

namespace {
    std::optional<double> number_param(const vda5050::Action& action, const std::string& key) {
        if (!action.action_parameters) {
            return std::nullopt;
        }
        for (const auto& p : *action.action_parameters) {
            if (p.key == key && p.value.is_number()) {
                return p.value.get<double>();
            }
        }
        return std::nullopt;
    }

    class SimulatedAction : public RunningAction {
    public:
        explicit SimulatedAction(double duration) : duration_(duration) {}

        vda5050::ActionStatus update(double dt) override {
            if (!paused_) {
                elapsed_ += dt;
            }
            return elapsed_ >= duration_ ? vda5050::ActionStatus::Finished
                                         : vda5050::ActionStatus::Running;
        }

        void pause() override { paused_ = true; }
        void resume() override { paused_ = false; }
        // cancel() — по умолчанию true: отменяется сразу (factsheet: cancelAllowed = true).

    private:
        double duration_;
        double elapsed_ = 0.0;
        bool paused_ = false;
    };

}  // namespace

void register_simulated_action(ActionExecutor& executor, double default_duration) {
    executor.register_handler(
        sim_action::kType,
        [default_duration](const vda5050::Action& action, vda5050::State&) -> std::unique_ptr<RunningAction> {
            const double duration = std::max(0.0, number_param(action, "duration").value_or(default_duration));
            return std::make_unique<SimulatedAction>(duration);
        });
}