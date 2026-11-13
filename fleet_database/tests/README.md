# Тесты fleet_database

Тесты работают **вместо fleet_control и fleet_dispatch**: вызывают gRPC `RobotRegistry`, `DispatchStorage`
и `ControlStorage` и проверяют ответы.
Нужен только запущенный `fleet_database` (и PostgreSQL за ним). Брокер и симулятор не нужны.

Python-классы gRPC генерируются из `proto/*.proto` при каждом запуске (в `_gen/`), отдельный шаг не нужен.

## Таблицу не чистим

В базе могут быть чужие роботы — тесты их не видят и не трогают. Каждый тест создаёт своих роботов
с уникальными `robot_id` и `serial_number` (`test-<случайное>`, производитель `test`), из `ListRobots`
смотрит только на них и в конце удаляет их через API (`SetRobotDeleting` + `DeleteRobot`).
Поэтому тесты можно гонять на той же базе, с которой работает dispatch, и повторять сколько угодно.

Карты — так же: `test-map-<случайное>`, из `ListMaps` тест смотрит только на свои, в конце удаляет их `DeleteMap`.

Миссии удалить через API нельзя — история не удаляется. Их id — `test-mission-<случайное>`, и в конце прогона
conftest удаляет их через `psql` (адрес базы — `DATABASE_URL`, по умолчанию `postgresql://fms:fms@127.0.0.1:5432/fms`).
Это важно: иначе control при старте загрузит незавершённые тестовые миссии как настоящие. Если `psql` не нашёлся
или не смог подключиться, pytest напишет предупреждение — удалите их сами (команда ниже).

Если тест упал на середине, его роботы всё равно удаляются. Если оборвать сам прогон (Ctrl+C),
могут остаться записи с производителем `test`:

```bash
psql postgresql://fms:fms@127.0.0.1:5432/fms -c "DELETE FROM robots WHERE manufacturer = 'test'"
```

```bash
psql postgresql://fms:fms@127.0.0.1:5432/fms -c "DELETE FROM maps WHERE map_id LIKE 'test-map-%'"
```

```bash
psql postgresql://fms:fms@127.0.0.1:5432/fms -c "DELETE FROM missions WHERE mission_id LIKE 'test-mission-%'"
```

## Установка

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Запуск

Из папки `fleet_database/tests`, при запущенном `fleet_database`:

```bash
.venv/bin/python -m pytest -v
```

Другой адрес сервиса — `DATABASE_ADDRESS` (gRPC, по умолчанию `localhost:5002`) и `DATABASE_HTTP` (REST, по умолчанию `http://localhost:8002`).

## Что проверяется

| Файл | Что |
|---|---|
| `test_robot_registry.py` | `CreateRobot`: что сохранилось, игнор `lifecycle` и времени, кавычки и юникод в имени и метках, занятый `robot_id`, тот же физический робот под другим id, обязательные поля, одновременные создания; `GetRobot` / `ListRobots`: совпадение с созданным, сортировка, `NOT_FOUND`; `UpdateRobot`: меняет только имя и метки (метки целиком), растёт `updated_at`; удаление в два шага: `FAILED_PRECONDITION` без пометки, скрытие `PENDING_DELETE` из списка, повторная пометка, повторная регистрация после удаления |
| `test_dispatch_storage.py` | `SaveRobotStatus`: статус целиком возвращается в `GetRobot` и `ListRobots`, новый заменяет старый полностью, запись для `PENDING_DELETE`, неизвестный робот → `NOT_FOUND`; `SaveFactsheet` / `GetFactsheet`: сохранить и прочитать, замена, время по умолчанию, не JSON → `INVALID_ARGUMENT`, нет factsheet → `NOT_FOUND`; статус и factsheet удаляются вместе с роботом |
| `test_robot_availability.py` | `SaveRobotAvailability` (пишет control): сохранить и прочитать через `GetRobot` и `ListRobots`, новая заменяет старую, spec не трогает, `DELETING` для робота на удалении, неизвестный робот → `NOT_FOUND`, без id или состояния → `INVALID_ARGUMENT`, удаляется вместе с роботом; в REST — `{state, reasons}` по контракту, без записи — `null` |
| `test_maps.py` | `ControlStorage` (пишет control): `CreateMap` — версия 1, документ сохраняется как есть, занятый id, обязательные поля, не JSON; `GetMap` с картинкой, `ListMaps` с графом, но без картинки, по `map_id`; `UpdateMap` — версия + 1, картинка прежняя без `new_image`, замена и очистка картинки, устаревшая версия → `ABORTED` без изменений, 10 одновременных сохранений — применяется одно; `DeleteMap`; карта на 5 МБ проходит, на 40 МБ — `RESOURCE_EXHAUSTED` (лимит сервера 32 МБ); REST: список без графа и картинки, карта целиком, `pgmDataUrl: null` без картинки, новая версия, 404 |
| `test_missions.py` | `ControlStorage` (пишет control): `SaveMission` — сохранить и прочитать как есть (времена от control, пустой робот ↔ NULL), замена записи целиком, снятие назначения, неверные `state` / `priority` и не JSON → `INVALID_ARGUMENT` без записи, обязательные поля; `GetMission` → `NOT_FOUND`; `ListActiveMissions` — только незавершённые, по `created_at`, завершённая уходит из списка; REST: миссия — документ control как есть, 404, список новыми сверху, фильтры `state` (и через `%2C`), `robotId`, `mapId` и их сочетание, страницы по курсору без пропусков и повторов (и при одинаковом `created_at`), последняя полная страница без курсора, `limit` по умолчанию 50 и до 200, ошибки параметров 400 с `details`; период `from`/`to` по `createdAt` — полуинтервал [from, to), время со смещением и долями (`%2B`), страницы внутри периода, неверное время и `to` не позже `from` → 400. События (c3): пишутся в `SaveMission` вместе с миссией, `GET /missions/{id}/events` — поля строки (`id`, `missionId`, `at` ISO с мс, `robotId`, `type`) + `details` как есть, дописываются по порядку записи, вложенные объекты сохраняются, `details` не подменяет поля строки, пустые `details`; нет событий — `[]`, нет миссии — 404; неверное событие (тип, нет `at`, `details` не объект или не JSON) → `INVALID_ARGUMENT`, не записано ничего; миссия не записалась — её события тоже; удаление миссии уносит события (через `psql`) |
| `test_rest.py` | REST для UI: робот в формате контракта (поля, `name` по умолчанию = id, `PENDING_DELETE` в списке), статус (enum без префиксов, `null` вместо пустых значений, время с миллисекундами), factsheet, 404 в формате `{code, message, details}`, CORS preflight, методов записи нет |

Что пока не автоматизировано: данные переживают перезапуск сервиса и `UNAVAILABLE` при остановленном
PostgreSQL — проверяются руками (см. [README сервиса](../README.md)).
