# Тесты fleet_control

Тесты работают **вместо UI**: вызывают REST control (`/api/fms/*`) и проверяют, что получилось в
fleet_database (через её REST и gRPC) и в fleet_dispatch (gRPC).

Нужны запущенные:

1. `fleet_control`;
2. `fleet_database` (и PostgreSQL за ним);
3. `fleet_dispatch`.

Большинству тестов брокер и симулятор не нужны: роботы регистрируются с выдуманными `serialNumber`,
dispatch просто подписывается на их топики. Тесты с живым роботом (вся цепочка control → база → dispatch →
робот) регистрируют робота симулятора `ktc/amr1`; если он не выходит на связь за 10 с, они пропускаются.
Перед ними `ktc/amr1` не должен быть зарегистрирован в базе под другим id.

Карта робота симулятора (`map_1`): если её нет в fms, тесты создают её сами и после удаляют; если есть —
берут как есть (нужен хотя бы один waypoint) и не меняют, а `test_availability_follows_maps` пропускается.

Python-классы gRPC генерируются из `proto/*.proto` при каждом запуске (в `_gen/`), отдельный шаг не нужен.

## Таблицу не чистим

У каждого теста свои роботы `test-ctl-<случайное>` (производитель `test`). После теста они удаляются из
dispatch и из базы, чужие записи тесты не трогают. Если прогон оборвать (Ctrl+C), могут остаться записи
с производителем `test`:

```bash
psql postgresql://fms:fms@127.0.0.1:5432/fms -c "DELETE FROM robots WHERE manufacturer = 'test'"
```

Карты — так же: `test-ctl-map-<случайное>`, удаляются через `DELETE` control (чтобы и граф в памяти control
не остался). Оставшиеся после обрыва — удалить и перезапустить control:

```bash
psql postgresql://fms:fms@127.0.0.1:5432/fms -c "DELETE FROM maps WHERE map_id LIKE 'test-ctl-map%' OR map_id ~ '^map-[a-z0-9]{6}$'"
```

## Установка

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Запуск

Из папки `fleet_control/tests`:

```bash
.venv/bin/python -m pytest -v
```

## Настройки (переменные окружения)

| Переменная | По умолчанию | Что это |
|---|---|---|
| `CONTROL_HTTP` | `http://localhost:8003` | REST fleet_control |
| `DATABASE_HTTP` | `http://localhost:8002` | REST fleet_database |
| `DATABASE_ADDRESS` | `localhost:5002` | gRPC fleet_database |
| `DISPATCH_ADDRESS` | `localhost:5001` | gRPC fleet_dispatch |
| `VDA_MANUFACTURER` / `VDA_SERIAL` | `ktc` / `amr1` | Робот симулятора для тестов с живым роботом |
| `VDA_MAP_ID` | `map_1` | Карта робота симулятора |

## Что проверяется

| Файл | Что |
|---|---|
| `test_robots.py` | `POST /robots`: ответ 201 в формате контракта, робот в базе и в dispatch, `name` по умолчанию = id, проверка полей (400 `INVALID_INPUT` с `details`, все ошибки сразу), не JSON, 409 на занятый id и тот же физический робот; `PATCH /robots/{id}`: имя и метки, неприсланное не меняется, `name: null` → id, `id`/`manufacturer`/`serialNumber` не меняются (400), неизвестный робот → 404 |
| `test_robot_commands.py` | `DELETE /robots/{id}`: 202 с `PENDING_DELETE`, после этого робота нет ни в базе, ни в dispatch, повторный DELETE → 404, можно зарегистрировать заново; pause/resume неизвестного робота → 404. С симулятором: зарегистрированный робот выходит ONLINE в базе, pause → `status.paused`, resume; DELETE при миссии → 409 `ROBOT_BUSY`, с `?force=true` → 202 и миссия `FAILED ROBOT_REMOVED` |
| `test_fleet_sync.py` | Сверка с dispatch (каждый тест до ~15 с): робот, записанный в базу в обход control, появляется в dispatch; удаление, прерванное на `PENDING_DELETE`, доводится до конца — робота нет ни в базе, ни в dispatch |
| `test_maps.py` | Карты (M-1): `POST /maps` — 201, в базе то же, что в ответе; свой id (= mapId робота) или сгенерированный `map-xxxxxx`; 409 на занятый id; поля → 400 `INVALID_INPUT`; каждая ошибка графа → 400 `INVALID_MAP` с текстом и все ошибки сразу, ничего не сохранено; предупреждения не мешают; пустая карта; неизвестные поля документа сохраняются; `version`, время, `zoneSet` ставит сервер. `PATCH` — частичная замена, картинка прежняя, замена и удаление картинки, устаревшая версия → 409 `VERSION_CONFLICT` с `currentVersion`, 5 одновременных PATCH — проходит один, проверка полей и графа, id не меняется, 404. `DELETE` — 204, потом 404, можно создать заново; с симулятором — карта, на которой стоит робот, → 409 `INVALID_STATE`. Карта на 5 МБ — POST и PATCH (лимиты HTTP и gRPC) |
| `test_availability.py` | Готовность робота в `GET` базы: выдуманный робот (никогда не выходил на связь) — `OFFLINE` с причиной, availability записана сразу после регистрации. С симулятором: робот на узле графа — `IDLE` без причин, pause → `PAUSED`, resume → `IDLE`, миссия → `BUSY` |
