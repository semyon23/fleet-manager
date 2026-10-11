# Action'ы в order и instant action'ы для RoboticVehicleSimulator

Что даёт: action'ы на узлах и рёбрах order'а (`pick`, `drop`, свои) по правилам
`blockingType`, пауза, отмена order'а, `stateRequest`, `factsheetRequest`,
`initializePosition` (VDA 5050 v3.0.0, 6.1.3, 6.2, 6.6.9).

## Состав папки

| Файл | Что делать |
|---|---|
| `include/ActionExecutor.hpp`, `src/ActionExecutor.cpp` | **Добавить в проект.** Очередь action'ов, статусы в `actionStates` / `instantActionStates` |
| `include/PickDropActions.hpp`, `src/PickDropActions.cpp` | **Добавить.** Обработчики `pick` / `drop`. `LoadHandlingConfig` берётся из `RobotConfig.hpp` |
| `src/RoboticVehicleSimulatorInstantActions.cpp` | **Добавить.** Методы `RoboticVehicleSimulator` для instant action'ов |
| `include/action_nodes.hpp`, `src/action_nodes.cpp` | **Заменить свои.** Исправлен `ExecuteNodeActions::onHalted` (пауза больше не роняет action'ы в FAILED) |
| `reference/RobotSimulatorBehavior.cpp` | **Не компилировать.** Образец методов `RobotBehaviorApi`, откуда переносить куски (шаг 6) |
| `reference/RobotSimulatorNodeTraversal.cpp` | **Не компилировать.** Образец `on_node_reached` (шаг 5) |
| `reference/robot_tree.xml` | Дерево с `IsPaused` / `HoldPosition` / `ExecuteNodeActions` — сверить со своим |

> **Имена полей.** Файлы в `reference/` писались под старый `RobotSimulator`:
> там `State`, `Order`, `config`, `drive_controller`, `emit_state_changed()`.
> `RoboticVehicleSimulatorInstantActions.cpp` — под новый класс:
> `State_`, `Order_`, `config_`, `driver_`, `publisher_`.
> При переносе подставь имена из своего класса.

---

## Шаг 1. CMake

```cmake
target_sources(robot_simulator PRIVATE
        src/ActionExecutor.cpp
        ../src/PickDropActions.cpp
        src/RoboticVehicleSimulatorInstantActions.cpp
        src/action_nodes.cpp            # заменить существующий
)
```

## Шаг 2. `RoboticVehicleSimulator.hpp`

```cpp
#include "ActionExecutor.hpp"
#include "PickDropActions.hpp"
```

Поля. `actions_` хранит ссылку на `State_`, поэтому объявить **после** `State_`:

```cpp
vda5050::State State_;
vda5050::Order Order_;
bool order_canceled_ = false;                    // если ещё нет (его же ставит update_order)

ActionExecutor actions_{State_};
std::optional<std::string> pending_cancel_id_;   // cancelOrder ждёт завершения action'ов

// уже есть у тебя, если строишь сегменты как в reference/RobotSimulatorBehavior.cpp:
std::uint32_t segment_end_seq_ = 0;
bool segment_ends_blocking_ = false;
```

Методы:

```cpp
public:
    // Вызывать в потоке симуляции (см. шаг 8)
    void handle_instant_actions(const std::string& json);

private:
    void execute_instant_action(const vda5050::Action& a);
    void cancel_order(const vda5050::Action& a);
    void set_paused(const vda5050::Action& a, bool paused);
    void initialize_position(const vda5050::Action& a);
    void update_pending_cancel();
    void set_instant_status(const std::string& action_id, vda5050::ActionStatus status);
    void add_warning(const std::string& type, const std::string& description,
                     std::vector<vda5050::ErrorReference> refs);
```

## Шаг 3. Конструктор — зарегистрировать обработчики

```cpp
register_pick_drop(actions_, config_.load_handling);
```

Свои action'ы — так же: `actions_.register_handler("myAction", factory)`
(`factory` возвращает `std::unique_ptr<RunningAction>`, пример — `PickDropActions.cpp`).

Если при приёме order'а проверяешь, что action'ы поддерживаются
(`validate_capabilities`), список бери из `actions_.supported_types()`.
В factsheet (`protocolFeatures.agvActions`) — тоже их.

## Шаг 4. Приём order'а (`update_order`)

`actionStates` со статусом WAITING уже заполняет `append_states` — не трогать.

**Новый order** (не update), перед `State_.action_states.clear()`:

```cpp
actions_.cancel_all_order_actions();   // выкинуть из очереди хвосты прошлого order'а
```

Первый узел нового order'а отдельно ставить в очередь не нужно: это делает
`start_node_actions()` из дерева (`ExecuteNodeActions`).

## Шаг 5. Проход узла (`on_node_reached`)

Образец — `reference/RobotSimulatorNodeTraversal.cpp`. Добавить три вызова:

```cpp
// 1. съехали с ребра, которое вело к узлу: работающие -> FINISHED, не начатые -> FAILED
if (const auto* edge_in = find_edge(seq - 1)) {
    actions_.finish(edge_in->actions);
}
// ... удаление edgeStates / nodeStates, lastNodeId, lastNodeSequenceId — как у тебя ...

// 3. action'ы узла в очередь
actions_.enqueue(node->actions);

// 4. въехали на следующее ребро — его action'ы в очередь
if (const auto* edge_out = find_edge(seq + 1); edge_out && edge_out->released) {
    actions_.enqueue(edge_out->actions);
}
```

## Шаг 6. Методы `RobotBehaviorApi` (для дерева)

Образец — `reference/RobotSimulatorBehavior.cpp`.

**`start_segment()`** — первой строкой:

```cpp
if (actions_.blocks_driving()) {   // SOFT/HARD в работе или в очереди — ехать нельзя
    return false;
}
```

**`build_segment()`** — сегмент обрывается на узле с SOFT/HARD action
(функция `is_blocking` в начале файла):

```cpp
wp.stop = is_blocking(node);
...
segment_end_seq_ = node.sequence_id;
segment_ends_blocking_ = wps.back().stop;
if (segment_ends_blocking_) break;     // на этом узле обязаны остановиться
```

**`extend_segment()`** — в начале:

```cpp
if (segment_ends_blocking_ || driver_.path_finished()) return false;
```

**Action'ы узла** — перенести целиком:

```cpp
const vda5050::Node* RoboticVehicleSimulator::current_node() const;   // узел с lastNodeSequenceId
void RoboticVehicleSimulator::start_node_actions()      { if (auto* n = current_node()) actions_.enqueue(n->actions); }
bool RoboticVehicleSimulator::node_actions_done() const { auto* n = current_node(); return !n || actions_.is_done(n->actions); }
void RoboticVehicleSimulator::cancel_node_actions()     { if (auto* n = current_node()) actions_.cancel(n->actions); }
```

`enqueue()` пропускает уже стоящие в очереди action'ы — повторный вызов безопасен.

## Шаг 7. `simulation_tick(dt)` — порядок

```cpp
driver_.update_position(dt);              // внутри — on_node_reached()
// поза / скорость / driving в State_
if (actions_.update(dt)) {                // запуск по blockingType, продвижение pick/drop
    publisher_.emit();
}
update_pending_cancel();                  // cancelOrder -> FINISHED, когда доработали неотменяемые
tree_.tickOnce();
publisher_.emit();
```

`actions_.update()` — **до** дерева: тогда `ExecuteNodeActions` на этом же тике видит,
что action'ы закончились, и робот трогается без лишней задержки.

## Шаг 8. Подписка на `instantActions`

Рядом с подпиской на `order` (топик — `topics_.instant_actions` или как у тебя):

```cpp
// колбэк MQTT приходит не в потоке симуляции — перейти в него
asio::post(executor(), [self = shared_from_this(), msg = std::move(payload)] {
    self->handle_instant_actions(msg);
});
```

## Шаг 9. Что нужно от других классов

| Где | Метод | Зачем |
|---|---|---|
| `StatePublisher` | `emit()` | state при изменении |
| `StatePublisher` | `publish_state_now()` | `stateRequest` |
| `StatePublisher` | `publish_factsheet()` | `factsheetRequest` |
| контроллер движения | `standstill()` | `cancelOrder`, пауза |
| контроллер движения | `set_pose(x, y, theta)` | `initializePosition` |
| `RoboticVehicleSimulator` | `stop_driving()` | уже есть (`RobotBehaviorApi`) |
| `RobotConfig` | `name()`, `load_handling` | лог предупреждений, pick/drop |

Если `config_.name()` у тебя поле, а не метод — поправь последнюю строку `add_warning`.

## Шаг 10. Дерево

В `robot_tree.xml` должны быть: `ReactiveFallback` с `IsPaused` → `HoldPosition`
(пауза проверяется каждый тик) и `ExecuteNodeActions` после `DriveSegment`.
Сверь со своим по `reference/robot_tree.xml`.

---

## Как ведут себя instant action'ы

| Action | Что делает робот | Статус |
|---|---|---|
| `cancelOrder` | стоп, отмена action'ов order'а (неотменяемые дорабатывают), очистка `nodeStates`/`edgeStates`, `orderId` сохраняется | RUNNING до завершения всех action'ов, затем FINISHED. Нет order'а → FAILED + `NO_ORDER_TO_CANCEL` |
| `startPause` / `stopPause` | `paused`, `pause_all()` / `resume_all()`; движение останавливает дерево (`HoldPosition`) | FINISHED |
| `stateRequest` | state сразу | FINISHED |
| `factsheetRequest` | factsheet сразу | FINISHED |
| `initializePosition` | поза, `mapId`, `lastNodeId`, `localized`, `driver_.set_pose` | FINISHED; без параметров / есть order / карта не ENABLED → FAILED + `INVALID_INSTANT_ACTION` |
| свой зарегистрированный | через исполнитель как instant | по ходу выполнения |
| неизвестный | — | FAILED + `INVALID_INSTANT_ACTION` |
| повтор того же `actionId` | игнорируется | — |
| битый JSON | — | warning `VALIDATION_FAILURE` |

## Проверка после сборки

1. Order из 3 узлов, на среднем `pick` с `blockingType: HARD`.
   Робот останавливается на нём, `pick`: WAITING → RUNNING → FINISHED, в `loads` груз,
   робот едет дальше.
2. Во время `pick` — `startPause`: `pick` PAUSED, время не идёт. `stopPause` — RUNNING → FINISHED.
3. Во время `pick` — `cancelOrder`: движение остановлено, маршрут очищен, `pick` дорабатывает,
   `cancelOrder` RUNNING → FINISHED, робот idle.
4. `cancelOrder` без order'а → FAILED + `NO_ORDER_TO_CANCEL` в `errors`.
5. Order с action'ом `drop` на ребре (`blockingType: NONE`): выполняется на ходу.
