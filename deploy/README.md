# Развёртывание fms

## Шлюз nginx

UI обращается к fms по одному адресу `http://<машина>:8080/api`. Шлюз [`nginx/fms.conf`](nginx/fms.conf)
разводит запросы так, как требует контракт UI (раздел 1):

| Запрос | Куда |
|---|---|
| `GET /api/fms/*` | fleet_database, `127.0.0.1:8002` — только чтение |
| остальное `/api/*` | fleet_control, `127.0.0.1:8003` — действия и `/api/health` |

Установка (Ubuntu / WSL):

```bash
sudo apt install nginx
```

```bash
sudo cp deploy/nginx/fms.conf /etc/nginx/conf.d/fms.conf
```

```bash
sudo nginx -t && sudo service nginx restart
```

Проверка — через шлюз отвечают оба сервиса:

```bash
curl -s -w '\n' http://127.0.0.1:8080/api/health
```

```bash
curl -s -w '\n' http://127.0.0.1:8080/api/fms/robots
```

Первая команда отвечает fleet_control (`{"services": …, "status": "OK"}`), вторая — fleet_database (список роботов).

В UI: `apiBaseUrl` = `http://<машина>:8080/api` в `public/config.js`.

Сейчас в шлюзе нет проверки JWT: открывать порт 8080 наружу нельзя, только в локальной сети стенда.
Позже — проверка JWT, раздача собранного UI и Docker-образы сервисов.
