#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "vda5050.hpp"

class RunningAction {
public:
    virtual ~RunningAction() = default;

    // Продвинуть на dt секунд. Возвращает Running, Finished или Failed.
    virtual vda5050::ActionStatus update(double dt) = 0;

    virtual void pause() {}
    virtual void resume() {}
    // false — отменить нельзя (cancelAllowed = false), action доработает сам.
    virtual bool cancel() { return true; }

    // actionResult в state (например, прочитанный штрихкод). Пусто — нет результата.
    virtual std::optional<std::string> result() const { return std::nullopt; }
    // Описание причины FAILED — уйдёт в errors с ссылкой на actionId.
    virtual std::optional<std::string> failure_reason() const { return std::nullopt; }
};

// Создаёт RunningAction для конкретного action'а. Один обработчик на actionType.
// state — общий State робота (обработчики меняют loads и т.п.).
using ActionFactory =
    std::function<std::unique_ptr<RunningAction>(const vda5050::Action& a, vda5050::State& s)>;

class ActionExecutor {
public:
    explicit ActionExecutor(vda5050::State& state) : state_(state) {}

    void register_handler(const std::string& action_type, ActionFactory factory);
    std::set<std::string> supported_types() const;

    // Поставить action'ы в очередь. Они уже должны быть в actionStates / instantActionStates
    // со статусом WAITING (для order'а это делает приём order'а).
    void enqueue(const std::vector<vda5050::Action>& actions, bool instant = false);

    // Каждый тик. true — какой-то статус изменился (нужно отправить state).
    bool update(double dt);

    bool is_done(const std::vector<vda5050::Action>& actions) const;
    bool blocks_driving() const;

    // Съезд с ребра (6.2): работающие -> FINISHED, так и не начатые -> FAILED.
    void finish(const std::vector<vda5050::Action>& actions);
    // Отмена: WAITING -> FAILED, работающие с cancelAllowed -> FAILED.
    void cancel(const std::vector<vda5050::Action>& actions);
    void cancel_all_order_actions();

    void pause_all();
    void resume_all();

private:
    struct Entry {
        vda5050::Action action;
        bool instant = false;
        std::unique_ptr<RunningAction> running;  // пусто — ещё в очереди
        bool done = false;                        // завершён, удалить из очереди
    };

    vda5050::ActionState* find_state(const std::string& action_id, bool instant);
    const vda5050::ActionState* find_state(const std::string& action_id, bool instant) const;
    void set_status(Entry& e, vda5050::ActionStatus status);
    bool start(Entry& e);
    void report_failure(const Entry& e, const std::string& reason);

    vda5050::State& state_;
    std::map<std::string, ActionFactory> handlers_;
    std::vector<Entry> queue_;  // в порядке поступления (6.2.2)
    bool paused_ = false;
};
