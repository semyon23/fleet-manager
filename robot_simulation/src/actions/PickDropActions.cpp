#include "include/actions/PickDropActions.hpp"

#include <algorithm>
#include <memory>
#include <random>

namespace {

std::optional<std::string> string_param(const vda5050::Action& action, const std::string& key) {
    if (!action.action_parameters) {
        return std::nullopt;
    }
    for (const auto& p : *action.action_parameters) {
        if (p.key == key && p.value.is_string()) {
            return p.value.get<std::string>();
        }
    }
    return std::nullopt;
}

std::vector<vda5050::Load>& loads_of(vda5050::State& state) {
    if (!state.loads) {
        state.loads.emplace();  // робот умеет определять груз
    }
    return *state.loads;
}

// Груз на грузозахвате lhd. Без lhd — любой (у робота один грузозахват).
std::vector<vda5050::Load>::iterator find_load(std::vector<vda5050::Load>& loads,
                                               const std::optional<std::string>& lhd) {
    return std::find_if(loads.begin(), loads.end(), [&](const vda5050::Load& l) {
        return !lhd || l.load_position == lhd;
    });
}

class TimedLoadAction : public RunningAction {
public:
    TimedLoadAction(vda5050::State& state, const vda5050::Action& action, double duration,
                    bool will_fail)
        : state_(state),
          lhd_(string_param(action, "lhd")),
          load_id_(string_param(action, "loadId")),
          load_type_(string_param(action, "loadType")),
          duration_(duration),
          will_fail_(will_fail) {}

    vda5050::ActionStatus update(double dt) override {
        if (failure_) {
            return vda5050::ActionStatus::Failed;
        }
        if (!paused_) {
            elapsed_ += dt;
        }
        if (elapsed_ < duration_) {
            return vda5050::ActionStatus::Running;
        }
        if (will_fail_) {
            failure_ = failure_text();
            return vda5050::ActionStatus::Failed;
        }
        complete();
        return vda5050::ActionStatus::Finished;
    }

    void pause() override { paused_ = true; }
    void resume() override { paused_ = false; }
    // Груз уже в движении: прерывать нельзя (factsheet: cancelAllowed = false).
    bool cancel() override { return false; }

    std::optional<std::string> result() const override { return load_id_; }
    std::optional<std::string> failure_reason() const override { return failure_; }

protected:
    virtual void complete() = 0;
    virtual std::string failure_text() const = 0;

    vda5050::State& state_;
    std::optional<std::string> lhd_;
    std::optional<std::string> load_id_;
    std::optional<std::string> load_type_;
    std::optional<std::string> failure_;

private:
    double duration_;
    double elapsed_ = 0.0;
    bool paused_ = false;
    bool will_fail_;
};

class PickAction : public TimedLoadAction {
public:
    using TimedLoadAction::TimedLoadAction;

    void check_preconditions() {
        auto& loads = loads_of(state_);
        if (find_load(loads, lhd_) != loads.end()) {
            failure_ = "load handling device is occupied";
        }
    }

protected:
    void complete() override {
        vda5050::Load load;
        load.load_id = load_id_;
        load.load_type = load_type_;
        load.load_position = lhd_;
        loads_of(state_).push_back(std::move(load));
    }
    std::string failure_text() const override { return "load not detected at pick station"; }
};

class DropAction : public TimedLoadAction {
public:
    using TimedLoadAction::TimedLoadAction;

    void check_preconditions() {
        auto& loads = loads_of(state_);
        if (find_load(loads, lhd_) == loads.end()) {
            failure_ = "no load to drop";
        }
    }

protected:
    void complete() override {
        auto& loads = loads_of(state_);
        if (auto it = find_load(loads, lhd_); it != loads.end()) {
            if (!load_id_) {
                load_id_ = it->load_id;  // результат drop — какой груз положили
            }
            loads.erase(it);
        }
    }
    std::string failure_text() const override { return "drop station is occupied"; }
};

}  // namespace

void register_pick_drop(ActionExecutor& executor, const LoadHandlingConfig& config) {
    auto rng = std::make_shared<std::mt19937>(config.seed);

    auto duration = [rng, jitter = config.time_jitter](double base) {
        std::uniform_real_distribution<double> d(1.0 - jitter, 1.0 + jitter);
        return base * d(*rng);
    };
    auto will_fail = [rng, p = config.failure_probability] {
        return std::bernoulli_distribution(p)(*rng);
    };

    executor.register_handler(
        vda5050::action_type::kPick,
        [=](const vda5050::Action& action, vda5050::State& state) -> std::unique_ptr<RunningAction> {
            auto a = std::make_unique<PickAction>(state, action, duration(config.pick_time), will_fail());
            a->check_preconditions();
            return a;
        });

    executor.register_handler(
        vda5050::action_type::kDrop,
        [=](const vda5050::Action& action, vda5050::State& state) -> std::unique_ptr<RunningAction> {
            auto a = std::make_unique<DropAction>(state, action, duration(config.drop_time), will_fail());
            a->check_preconditions();
            return a;
        });
}
