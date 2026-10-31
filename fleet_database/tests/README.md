# Тесты fleet_database

Тесты работают **вместо fleet_control и fleet_dispatch**: вызывают gRPC `RobotRegistry` и `DispatchStorage`
и проверяют ответы.
Нужен только запущенный `fleet_database` (и PostgreSQL за ним). Брокер и симулятор не нужны.

Python-классы gRPC генерируются из `proto/*.proto` при каждом запуске (в `_gen/`), отдельный шаг не нужен.

## Таблицу не чистим

В базе могут быть чужие роботы — тесты их не видят и не трогают. Каждый тест создаёт своих роботов
с уникальными `robot_id` и `serial_number` (`test-<случайное>`, производитель `test`), из `ListRobots`
смотрит только на них и в конце удаляет их через API (`SetRobotDeleting` + `DeleteRobot`).
Поэтому тесты можно гонять на той же базе, с которой работает dispatch, и повторять сколько угодно.

Если тест упал на середине, его роботы всё равно удаляются. Если оборвать сам прогон (Ctrl+C),
могут остаться записи с производителем `test`:

```bash
psql postgresql://fms:fms@127.0.0.1:5432/fms -c "DELETE FROM robots WHERE manufacturer = 'test'"
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
| `test_rest.py` | REST для UI: робот в формате контракта (поля, `name` по умолчанию = id, `PENDING_DELETE` в списке), статус (enum без префиксов, `null` вместо пустых значений, время с миллисекундами), factsheet, 404 в формате `{code, message, details}`, CORS preflight, методов записи нет |

Что пока не автоматизировано: данные переживают перезапуск сервиса и `UNAVAILABLE` при остановленном
PostgreSQL — проверяются руками (см. [README сервиса](../README.md)).
