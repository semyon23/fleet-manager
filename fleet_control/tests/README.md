# Тесты fleet_control

Тесты работают **вместо UI**: вызывают REST control (`/api/fms/*`) и проверяют, что получилось в
fleet_database (через её REST и gRPC) и в fleet_dispatch (gRPC).

Нужны запущенные:

1. `fleet_control`;
2. `fleet_database` (и PostgreSQL за ним);
3. `fleet_dispatch`.

Брокер и симулятор этим тестам не нужны: роботы регистрируются с выдуманными `serialNumber`, dispatch
просто подписывается на их топики.

Python-классы gRPC генерируются из `proto/*.proto` при каждом запуске (в `_gen/`), отдельный шаг не нужен.

## Таблицу не чистим

У каждого теста свои роботы `test-ctl-<случайное>` (производитель `test`). После теста они удаляются из
dispatch и из базы, чужие записи тесты не трогают. Если прогон оборвать (Ctrl+C), могут остаться записи
с производителем `test`:

```bash
psql postgresql://fms:fms@127.0.0.1:5432/fms -c "DELETE FROM robots WHERE manufacturer = 'test'"
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

## Что проверяется

| Файл | Что |
|---|---|
| `test_robots.py` | `POST /robots`: ответ 201 в формате контракта, робот в базе и в dispatch, `name` по умолчанию = id, проверка полей (400 `INVALID_INPUT` с `details`, все ошибки сразу), не JSON, 409 на занятый id и тот же физический робот; `PATCH /robots/{id}`: имя и метки, неприсланное не меняется, `name: null` → id, `id`/`manufacturer`/`serialNumber` не меняются (400), неизвестный робот → 404 |
