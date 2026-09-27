# vda5050 — структуры сообщений VDA 5050 v3.0.0

Header-only, C++17, [nlohmann/json](https://github.com/nlohmann/json) 3.11+.
Спецификация: `fleet-manager/docs/VDA5050_EN.md` (номера разделов в комментариях — оттуда).

| Файл | Топик | Кто публикует | QoS |
|---|---|---|---|
| `order.hpp` | `order` | fleet control | 0 |
| `instant_actions.hpp` | `instantActions` | fleet control | 0 |
| `zone_set.hpp` | `zoneSet` (опц.) | fleet control | 0 |
| `responses.hpp` | `responses` (опц.) | fleet control | 0 |
| `state.hpp` | `state` | робот | 0 |
| `visualization.hpp` | `visualization` (опц.) | робот | 0 |
| `connection.hpp` | `connection` | робот / брокер (last will) | 1, retained |
| `factsheet.hpp` | `factsheet` | робот | 0, retained |
| `types.hpp` | — | общие типы: Action, NodePosition, Trajectory, MobileRobotPosition, ZoneType, ... | |
| `common.hpp` | — | Header, хелперы сериализации, `make_timestamp()` | |
| `vda5050.hpp` | — | всё одним include | |

Топики (`interfaceName/majorVersion/manufacturer/serialNumber/topic`) здесь не зашиты — согласуем отдельно.

## Использование

```cpp
#include "vda5050/vda5050.hpp"

// парсинг
vda5050::State state = nlohmann::json::parse(payload).get<vda5050::State>();
if (state.mobile_robot_position && state.mobile_robot_position->localized) { ... }

// отправка
vda5050::InstantActions msg;
msg.header = {++header_id_instant_actions, vda5050::make_timestamp(), vda5050::kProtocolVersion, "KIT", "0001"};
msg.actions.push_back({vda5050::action_type::kCancelOrder, uuid(), std::nullopt,
                       vda5050::BlockingType::None, std::nullopt, std::nullopt});
std::string payload = nlohmann::json(msg).dump();
```

## Отправка state по событиям (сравнение `==` / `!=`)

По 6.6 state шлётся сразу при событии (новый order, проход узла, смена actionStates, errors, loads, ...)
и не реже раза в 30 с. `State == State` сравнивает только поля-события (см. `eq_key(const State&)` в `state.hpp`),
поэтому «было ли событие» — это `state != last_sent`. Примеры ниже проверены: собираются с `-std=c++17` + Boost.Asio.

### Робот / симулятор: отправка при событии + раз в 30 с

```cpp
#include <boost/asio.hpp>
#include "vda5050/vda5050.hpp"

namespace asio = boost::asio;
using PublishFn = std::function<void(const std::string& topic, const std::string& payload)>;

class StatePublisher {
public:
    StatePublisher(asio::io_context& io, std::string topic, PublishFn publish)
        : timer_(io), topic_(std::move(topic)), publish_(std::move(publish)) {}

    void start() { send(); }

    // Любое изменение state — только через update().
    // Поменялись поля-события — отправляем сразу, иначе ждём таймер.
    template <typename Fn>
    void update(Fn&& change) {
        change(state_);
        if (state_ != last_sent_) {
            send();
        }
    }

private:
    void send() {
        state_.header.header_id = ++header_id_;
        state_.header.timestamp = vda5050::make_timestamp();
        publish_(topic_, vda5050::json(state_).dump());
        last_sent_ = state_;

        // после любой отправки отсчёт 30 с начинается заново
        timer_.expires_after(std::chrono::seconds(30));
        timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec) send();   // operation_aborted — таймер переставили, это нормально
        });
    }

    asio::steady_timer timer_;
    std::string topic_;
    PublishFn publish_;
    vda5050::State state_;
    vda5050::State last_sent_;
    std::uint32_t header_id_ = 0;
};
```

Использование:

```cpp
StatePublisher pub(io, "vda5050/v3/KIT/0001/state",
                   [&](auto& topic, auto& payload) { mqtt.publish(topic, payload, /*qos*/ 0); });
pub.start();

// одометрия 50 Гц — пакет не уходит, позиция поедет со следующим
pub.update([&](vda5050::State& s) {
    s.mobile_robot_position = pos;
    s.velocity = vel;
    s.power_supply.state_of_charge = soc;
});

// прошли узел — пакет уходит сразу
pub.update([&](vda5050::State& s) {
    s.last_node_id = s.node_states.front().node_id;
    s.last_node_sequence_id = s.node_states.front().sequence_id;
    s.node_states.erase(s.node_states.begin());
});

// action завершилась — пакет уходит сразу
pub.update([&](vda5050::State& s) {
    for (auto& a : s.action_states)
        if (a.action_id == id) a.action_status = vda5050::ActionStatus::Finished;
});
```

### FMS: приём state и генерация событий

```cpp
struct RobotEvents {
    std::function<void(const std::string&, const vda5050::State&)> on_state_changed;
    std::function<void(const std::string&, const std::vector<vda5050::Error>&)> on_errors_changed;
    std::function<void(const std::string&, const vda5050::State&)> on_order_progress;
};

class FleetStateTracker {
public:
    explicit FleetStateTracker(RobotEvents ev) : ev_(std::move(ev)) {}

    // вызывать из MQTT-колбэка на топик .../state
    void on_state_message(const std::string& robot_id, const std::string& payload) {
        vda5050::State cur;
        try {
            cur = vda5050::json::parse(payload).get<vda5050::State>();
        } catch (const std::exception& e) {
            std::cerr << "bad state from " << robot_id << ": " << e.what() << '\n';
            return;
        }

        auto it = last_.find(robot_id);
        const bool first = it == last_.end();

        // периодический пакет (раз в 30 с) без изменений сюда не попадёт
        if (first || cur != it->second) {
            if (ev_.on_state_changed) ev_.on_state_changed(robot_id, cur);

            // у вложенных типов тоже есть ==, можно сравнивать отдельные поля
            if ((first || cur.errors != it->second.errors) && ev_.on_errors_changed)
                ev_.on_errors_changed(robot_id, cur.errors);           // -> алерты

            if ((first || cur.last_node_id != it->second.last_node_id ||
                 cur.action_states != it->second.action_states) && ev_.on_order_progress)
                ev_.on_order_progress(robot_id, cur);                  // -> диспетчер миссий
        }

        last_[robot_id] = std::move(cur);
    }

private:
    RobotEvents ev_;
    std::map<std::string, vda5050::State> last_;
};
```

### Нюансы

- `update()` и таймер — в одном потоке `io_context` или через один `strand`. Одометрия из другого потока —
  `asio::post(strand, [&]{ pub.update(...); })`.
- Позицию на стороне FMS брать из **каждого** сообщения, а не только при `!=` — `==` её не сравнивает.
  Для частой позиции есть топик `visualization`.
- Новое поле-событие — добавить его в `eq_key(const State&)` в `state.hpp`. Сравнение своей структуры —
  объявить рядом `inline auto eq_key(const T& v) { return std::tie(v.a, v.b); }`.

## Соглашения

- **Имена.** Поля в C++ — `snake_case`, в JSON — `camelCase` как в спецификации (`order_update_id` ↔ `orderUpdateId`).
- **Header** лежит в каждом сообщении полем `header`, в JSON его поля на верхнем уровне (так в спецификации). `headerId` — свой счётчик на каждый топик.
- **Optional-поля** — `std::optional`. `nullopt` не пишется в JSON вообще; при чтении отсутствующее поле или `null` → `nullopt`.
  Для массивов это важно: `state.loads == nullopt` — робот не умеет определять груз, пустой вектор — умеет, но груза нет.
- **Обязательные поля**: если поля нет или тип не тот — исключение `nlohmann::json::exception`
  (`out_of_range.403`, `type_error.302`). Чтобы в тексте был путь до поля, собирать с `-DJSON_DIAGNOSTICS=1`.
- **Закрытые enum'ы** (`BlockingType`, `OperatingMode`, `ErrorLevel`, ...) — `enum class`.
  Неизвестная строка → `std::invalid_argument` ("unknown ConnectionState value 'CONNECTIONBROKEN'"), а не тихий дефолт,
  как у `NLOHMANN_JSON_SERIALIZE_ENUM`. Для логов есть `to_string(enum)`.
- **Расширяемые enum'ы** (`actionType`, `errorType`, `mobileRobotClass`, `mobileRobotKinematics`, типы локализации,
  навигации, колёс) — `std::string`, производитель может прислать своё. Предопределённые значения — константы
  `action_type::k*`, `error_type::k*`, `mobile_robot_class::k*` и т.д.
- `is_idle(state)` — idle по 6.6.8 (нет nodeStates/edgeStates, все actionStates FINISHED/FAILED).
- **Сравнение** `==`/`!=`: у структуры есть `eq_key()` со списком полей (шаблон в `common.hpp`).
  `State` сравнивается только по полям-событиям из 6.6 (order, lastNode, node/edge/actionStates, loads, errors,
  driving, paused, operatingMode, safetyState, charging, requests, maps, zoneSets) — без header, позиции, скорости и заряда.
  Отправка: `if (state != last_sent) publish(state);` плюс таймер 30 с.

## Места, где спецификация сама себе противоречит

- `responses`: в 6.4.3 поле `responseType`, в таблице 7.5 — `grantType`. Пишем `grantType`, читаем оба.
- `requestStatus`: таблица 7.8 — REQUESTED/GRANTED/REVOKED/EXPIRED, раздел 6.9 добавляет QUEUED. Принимаем все пять.
- `errorType`: `INVALID_ORDER` есть только в перечне 7.8, в таблице 9 — `INVALID_ORDER_ACTION`. Константы есть обе.
- `factsheet.mobileRobotConfiguration`: разметка таблицы поехала, `batteryCharging` положен рядом с `versions` и `network`.

## Отличия от v2, на которые стоит смотреть при переносе

`agvPosition` → `mobileRobotPosition`, `positionInitialized` → `localized`, `batteryState` → `powerSupply`
(`batteryCharge` → `stateOfCharge`), `agvClass` → `mobileRobotClass`, `CONNECTIONBROKEN` → `CONNECTION_BROKEN`
(+ `HIBERNATING`), `loads` может отсутствовать, новые топики `zoneSet` и `responses`, запросы `zoneRequests`/`edgeRequests`,
`operatingMode` `INTERVENED` и `TEACH_IN`, уровни ошибок `URGENT` и `CRITICAL`.
