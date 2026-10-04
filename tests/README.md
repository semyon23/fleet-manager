.venv/bin/python -m pytest -v
#Тесты робота VDA 5050 v3 (без action'ов)

Тесты работают как «мини-fms»: подключаются к MQTT-брокеру, отправляют роботу
order'а и проверяют, что он отвечает в `state`, `connection`, `factsheet`.
Маршруты строятся от текущей позиции робота по прямой вдоль его курса, поэтому
заранее известная карта не нужна — но впереди робота должно быть 4–5 м свободного места.

## Установка

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Запуск

Симулятор должен быть запущен и подключён к тому же брокеру.

```bash
VDA_BROKER_HOST=192.168.0.105 VDA_SERIAL=sim_1 .venv/bin/python -m pytest -v
```

Только одна группа: `pytest -v test_horizon.py`. Остановиться на первой ошибке: `pytest -x`.

## Настройки (переменные окружения)

| Переменная | По умолчанию | Что это |
|---|---|---|
| `VDA_BROKER_HOST` / `VDA_BROKER_PORT` | `localhost` / `1883` | Брокер |
| `VDA_USER` / `VDA_PASSWORD` | — | Логин брокера, если нужен |
| `VDA_INTERFACE` | `amr` | Первый уровень топика (`amr/v3/...`) |
| `VDA_MAJOR` | `v3` | Версия в топике |
| `VDA_MANUFACTURER` / `VDA_SERIAL` | `KIT` / `sim_1` | Какой робот |
| `VDA_SECONDS_PER_METER` | `4` | Запас времени на метр пути (медленный робот — увеличить) |
| `VDA_TIMEOUT_SCALE` | `1.0` | Множитель всех таймаутов |
| `VDA_MAX_STATE_INTERVAL` | `30` | Максимальный интервал между state |
| `VDA_RUN_SLOW` | — | `1` — включить медленный тест «state не реже раза в 30 с» |

## Если тест упал

pytest показывает, какая проверка не прошла. Для ожиданий («робот должен был доехать до узла»)
в тексте ошибки — какое условие ждали и последний state робота.

Кроме того, для каждого упавшего теста в `logs/<имя теста>.log` сохраняется **вся переписка
с роботом** за время теста: что отправили (`->`) и что робот ответил (`<-`), с временем от начала
теста. По нему видно, на каком шаге робот повёл себя не так.

| Переменная | Что делает |
|---|---|
| `VDA_LOG_DIR` | Папка для логов (по умолчанию `logs`) |
| `VDA_LOG_ALL=1` | Писать лог для каждого теста, а не только для упавших |
| `VDA_LOG_VISUALIZATION=1` | Включать в лог `visualization` (по умолчанию нет: их много) |

Полезные ключи pytest: `-v` (имя каждого теста), `-x` (стоп на первой ошибке),
`-k horizon` (только тесты с этим словом в имени), `--tb=short` (короче трассировка).

## Что проверяется

| Файл | Что |
|---|---|
| `test_connection.py` | `connection` = ONLINE, retained, QoS 1; `factsheet` retained, обязательные разделы, `cancelOrder`/`startPause`/`stopPause` |
| `test_state.py` | Содержимое state, режим, локализация, карта, visualization |
| `test_orders.py` | Order доезжает до конца; первый узел не в `nodeStates`; повтор того же order'а игнорируется; update на ходу без остановки; update к завершённому order'у; order из одного узла |
| `test_horizon.py` | Остановка на decision point, horizon в `nodeStates`, `newBaseRequest`; не едет по horizon'у; update заменяет horizon; update убирает horizon |
| `test_rejections.py` | Отказы 6.1.4: битый JSON, нет поля, неверное число рёбер, released после horizon, неизвестный enum, старт далеко, неизвестная карта, `orderUpdateId ≠ 0`, `OTHER_ORDER_ACTIVE`, `OUTDATED_ORDER_UPDATE`, `SAME_ORDER_UPDATE_ID`, update не с decision point; ошибка снимается следующим принятым order'ом |

**Инварианты** (`invariants.py`) проверяются для **каждого** state во всех тестах:
`headerId` растёт на 1 в каждом топике, `manufacturer`/`serialNumber`/`version`/`timestamp`,
обязательные поля state, порядок `sequenceId` в `nodeStates`/`edgeStates`, released идут
сплошным префиксом, пройденных узлов нет в `nodeStates`, `lastNodeSequenceId` не уменьшается,
интервал между state не больше 30 с. Нарушение показывается как ERROR у теста, во время которого оно случилось.

Перед каждым тестом робот приводится в исходное состояние: если он застрял на decision point —
order завершается update'ом без horizon'а; затем ожидание idle и order из одного узла на месте
(он снимает ошибки отказа прошлого теста). Если робот не возвращается в idle, прогон
останавливается с причиной, чтобы остальные тесты не ждали таймауты.

## Требования к роботу

- `operatingMode` = AUTOMATIC, `mobileRobotPosition.localized` = true.
- Узлы order'а приходят с `nodePosition` и `allowedDeviationXY` 0.5 м.
- Где спецификация не задаёт тип ошибки (новый order с `orderUpdateId ≠ 0`, update не с decision point),
  тест проверяет только сам факт отказа.
