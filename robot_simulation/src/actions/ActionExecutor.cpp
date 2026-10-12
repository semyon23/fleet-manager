#include "include/actions/ActionExecutor.hpp"

#include <algorithm>
#include <iostream>
#include <spdlog/spdlog.h>

using vda5050::ActionStatus;
using vda5050::BlockingType;

namespace {

bool is_terminal(ActionStatus s) {
    return s == ActionStatus::Finished || s == ActionStatus::Failed;
}

bool is_exclusive(BlockingType b) {
    return b == BlockingType::Single || b == BlockingType::Hard;
}

bool stops_driving(BlockingType b) {  // SOFT / HARD — стоя
    return b == BlockingType::Soft || b == BlockingType::Hard;
}

}  // namespace

void ActionExecutor::register_handler(const std::string& action_type, ActionFactory factory) {
    handlers_[action_type] = std::move(factory);
}

std::set<std::string> ActionExecutor::supported_types() const {
    std::set<std::string> types;
    for (const auto& [type, _] : handlers_) {
        types.insert(type);
    }
    return types;
}

vda5050::ActionState* ActionExecutor::find_state(const std::string& action_id, bool instant) {
    auto& list = instant ? state_.instant_action_states : state_.action_states;
    for (auto& a : list) {
        if (a.action_id == action_id) {
            return &a;
        }
    }
    return nullptr;
}

const vda5050::ActionState* ActionExecutor::find_state(const std::string& action_id,
                                                       bool instant) const {
    const auto& list = instant ? state_.instant_action_states : state_.action_states;
    for (const auto& a : list) {
        if (a.action_id == action_id) {
            return &a;
        }
    }
    return nullptr;
}

void ActionExecutor::set_status(Entry& e, ActionStatus status) {
    if (auto* st = find_state(e.action.action_id, e.instant)) {
        st->action_status = status;
        if (e.running) {
            st->action_result = e.running->result();
        }
    }
}

void ActionExecutor::report_failure(const Entry& e, const std::string& reason) {
    spdlog::error("[actions] type : {}, id : {}, failed : {}",  e.action.action_type, e.action.action_id, reason);
    vda5050::Error err;
    err.error_type = "ACTION_FAILED";  // свой тип: спецификация позволяет добавлять
    err.error_level = vda5050::ErrorLevel::Warning;
    err.error_description = reason;
    err.error_references = std::vector<vda5050::ErrorReference>{
        {"actionId", e.action.action_id}, {"actionType", e.action.action_type}};
    state_.errors.push_back(std::move(err));
}

void ActionExecutor::enqueue(const std::vector<vda5050::Action>& actions, bool instant) {
    for (const auto& action : actions) {
        const auto* st = find_state(action.action_id, instant);
        if (!st || st->action_status != ActionStatus::Waiting) {
            continue;  // неизвестен или уже запускался
        }
        const bool queued = std::any_of(queue_.begin(), queue_.end(), [&](const Entry& e) {
            return e.action.action_id == action.action_id;
        });
        if (!queued) {
            queue_.push_back({action, instant, nullptr, false});
        }
    }
}

bool ActionExecutor::start(Entry& e) {
    const auto it = handlers_.find(e.action.action_type);
    if (it == handlers_.end()) {
        // В order такое не должно попасть (проверка по factsheet), но на всякий случай.
        set_status(e, ActionStatus::Failed);
        report_failure(e, "no handler for action type");
        e.done = true;  // иначе SOFT/HARD навсегда остался бы в очереди и блокировал езду
        return true;    // статус изменился
    }
    e.running = it->second(e.action, state_);
    spdlog::info("[actions] type : {}, id : {}, started",  e.action.action_type, e.action.action_id);

    set_status(e, ActionStatus::Running);
    if (paused_) {
        e.running->pause();
        set_status(e, ActionStatus::Paused);
    }
    return true;
}

bool ActionExecutor::update(double dt) {
    bool changed = false;

    for (auto& e : queue_) {
        if (!e.running || paused_) {
            continue;
        }
        const ActionStatus s = e.running->update(dt);
        if (s == ActionStatus::Finished || s == ActionStatus::Failed) {
            set_status(e, s);
            if (s == ActionStatus::Failed) {
                report_failure(e, e.running->failure_reason().value_or("action failed"));
            } else {
                spdlog::info("[actions] type : {}, id : {}, finished",  e.action.action_type, e.action.action_id);
            }
            e.running.reset();
            e.done = true;
            changed = true;
        }
    }

    // 2. Убрать завершённые из очереди (статус уже в State).
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [](const Entry& e) { return e.done; }),
                 queue_.end());

    // 3. Запуск по правилам blockingType (рисунок 11): очередь в порядке поступления,
    //    NONE/SOFT — пачкой параллельно, SINGLE/HARD — только когда больше ничего не работает,
    //    и после него ничего не запускается, пока он не закончится.
    //    Идёт после завершения, чтобы следующий action стартовал в том же тике.
    if (!paused_) {
        bool exclusive_running = std::any_of(queue_.begin(), queue_.end(), [](const Entry& e) {
            return e.running && is_exclusive(e.action.blocking_type);
        });
        bool anything_running = std::any_of(queue_.begin(), queue_.end(),
                                            [](const Entry& e) { return e.running != nullptr; });

        for (auto& e : queue_) {
            if (e.running) {
                continue;
            }
            if (stops_driving(e.action.blocking_type) && state_.driving) {
                break;   // SOFT/HARD — только стоя: стартует на следующем тике, когда робот встанет
            }

            if (exclusive_running) {
                break;
            }
            if (is_exclusive(e.action.blocking_type)) {
                if (anything_running) {
                    break;
                }
                changed = start(e) || changed;
                if (e.running) {
                    break;  // за работающим SINGLE/HARD никто не запускается
                }
                continue;   // не запустился (FAILED) — очередь идёт дальше
            }
            if (start(e)) {
                changed = true;
                anything_running = anything_running || e.running != nullptr;
            }
        }
        // Не запустившиеся (нет обработчика) уже FAILED — убрать.
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [](const Entry& e) { return e.done; }),
                     queue_.end());
    }
    return changed;
}

bool ActionExecutor::is_done(const std::vector<vda5050::Action>& actions) const {
    for (const auto& action : actions) {
        const auto* st = find_state(action.action_id, false);
        if (st && !is_terminal(st->action_status)) {
            return false;
        }
    }
    return true;
}

bool ActionExecutor::blocks_driving() const {
    return std::any_of(queue_.begin(), queue_.end(),
                       [](const Entry& e) { return stops_driving(e.action.blocking_type); });
}

void ActionExecutor::finish(const std::vector<vda5050::Action>& actions) {
    for (const auto& action : actions) {
        auto* st = find_state(action.action_id, false);
        if (!st || is_terminal(st->action_status)) {
            continue;
        }
        st->action_status =
            st->action_status == ActionStatus::Waiting ? ActionStatus::Failed : ActionStatus::Finished;
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [&](const Entry& e) {
                                        return e.action.action_id == action.action_id;
                                    }),
                     queue_.end());
    }
}

void ActionExecutor::cancel(const std::vector<vda5050::Action>& actions) {
    for (const auto& action : actions) {
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (it->action.action_id != action.action_id) {
                ++it;
                continue;
            }
            if (it->running && !it->running->cancel()) {
                ++it;  // cancelAllowed = false — доработает и сам станет FINISHED/FAILED
                continue;
            }
            set_status(*it, ActionStatus::Failed);
            it = queue_.erase(it);
        }
        // Ещё не в очереди (например, узел впереди) — просто FAILED.
        if (auto* st = find_state(action.action_id, false); st && st->action_status == ActionStatus::Waiting) {
            st->action_status = ActionStatus::Failed;
        }
    }
}

void ActionExecutor::cancel_all_order_actions() {
    std::vector<vda5050::Action> order_actions;
    for (const auto& e : queue_) {
        if (!e.instant) {
            order_actions.push_back(e.action);
        }
    }
    cancel(order_actions);
    for (auto& st : state_.action_states) {  // и те, что ещё не дошли до очереди
        if (st.action_status == ActionStatus::Waiting) {
            st.action_status = ActionStatus::Failed;
        }
    }
}

void ActionExecutor::pause_all() {
    if (paused_) {
        return;
    }
    paused_ = true;
    for (auto& e : queue_) {
        if (e.running) {
            e.running->pause();
            set_status(e, ActionStatus::Paused);
        }
    }
}

void ActionExecutor::resume_all() {
    if (!paused_) {
        return;
    }
    paused_ = false;
    for (auto& e : queue_) {
        if (e.running) {
            e.running->resume();
            set_status(e, ActionStatus::Running);
        }
    }
}
