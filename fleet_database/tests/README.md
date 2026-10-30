# Тесты fleet_database

Тесты работают **вместо fleet_control**: вызывают gRPC `RobotRegistry` и проверяют ответы.
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

Другой адрес сервиса — `DATABASE_ADDRESS` (по умолчанию `localhost:5002`).

## Что проверяется

| Файл | Что |
|---|---|
| `test_robot_registry.py` | `CreateRobot`: что сохранилось, игнор `lifecycle` и времени, кавычки и юникод в имени и метках, занятый `robot_id`, тот же физический робот под другим id, обязательные поля, одновременные создания; `GetRobot` / `ListRobots`: совпадение с созданным, сортировка, `NOT_FOUND`; `UpdateRobot`: меняет только имя и метки (метки целиком), растёт `updated_at`; удаление в два шага: `FAILED_PRECONDITION` без пометки, скрытие `PENDING_DELETE` из списка, повторная пометка, повторная регистрация после удаления |

Что пока не автоматизировано: данные переживают перезапуск сервиса и `UNAVAILABLE` при остановленном
PostgreSQL — проверяются руками (см. [README сервиса](../README.md)).
