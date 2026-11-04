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

## MQTT-брокер (mosquitto)

Конфиг [`mosquitto/fms.conf`](mosquitto/fms.conf) и права [`mosquitto/fms.acl`](mosquitto/fms.acl):

| Порт | Кто | Протокол |
|---|---|---|
| `1883` | роботы, fleet_dispatch, симулятор | MQTT |
| `9001` | веб-UI | MQTT over WebSocket |

Вход только по логину и паролю, у каждого — свои топики:

| Пользователь | Кто | Права |
|---|---|---|
| `fleet-dispatch` | fleet_dispatch | читает `state`, `visualization`, `connection`, `factsheet` роботов; пишет `order`, `instantActions`, а `connection` и `factsheet` — чтобы стирать retained-сообщения удалённого робота |
| `fleet-control` | fleet_control | пишет `fms/v1/#` — события fms для UI (позже) |
| `fleet-ui` | веб-UI | только чтение: `visualization`, `connection` роботов и `fms/v1/#` |
| `fms-dev` | симулятор и тесты | все топики роботов — только на стенде |
| `<serialNumber>` | настоящий робот | только свои топики (`%u` в ACL) |

Установка (Ubuntu / WSL):

```bash
sudo apt install mosquitto mosquitto-clients
```

```bash
sudo cp deploy/mosquitto/fms.conf /etc/mosquitto/conf.d/fms.conf && sudo cp deploy/mosquitto/fms.acl /etc/mosquitto/fms.acl
```

Пользователи. Пароль каждый раз спрашивается в терминале; в git и в логах его быть не должно. `-c` — только для первого
пользователя, он создаёт файл:

```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd fleet-dispatch
```

```bash
sudo mosquitto_passwd /etc/mosquitto/passwd fleet-ui
```

```bash
sudo mosquitto_passwd /etc/mosquitto/passwd fms-dev
```

Права на файлы и перезапуск:

```bash
sudo chown mosquitto:mosquitto /etc/mosquitto/passwd /etc/mosquitto/fms.acl && sudo chmod 0600 /etc/mosquitto/passwd /etc/mosquitto/fms.acl
```

```bash
sudo service mosquitto restart
```

На стенде, где порт 1883 занят (WSL с `netsh portproxy`), в `/etc/mosquitto/conf.d/fms.conf` поставить `listener 1884`.

Проверка — UI читает позицию, но не может отправить order (подставьте свои пароли):

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -u fleet-ui -P '<пароль fleet-ui>' -t 'amr/v3/+/+/visualization' -v -W 5
```

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -u fms-dev -P '<пароль fms-dev>' -t 'amr/v3/+/+/order' -v
```

и во втором терминале — публикация от имени UI, которая **не должна** дойти до подписчика выше:

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 -u fleet-ui -P '<пароль fleet-ui>' -t 'amr/v3/ktc/amr1/order' -m '{}'
```

Что поменять у клиентов брокера:
- **симулятор** — `mqtt.username` / `mqtt.password` в `robot_simulation/config/config.yaml`: `fms-dev`;
- **тесты** — `VDA_USER=fms-dev`, `VDA_PASSWORD=…`;
- **fleet_dispatch** — логин `fleet-dispatch`; адрес и логин брокера dispatch должен брать из переменных окружения
  (`MQTT_HOST`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASSWORD`);
- **веб-UI** — `mqttUrl` = `ws://<машина>:9001`, `mqttUser` = `fleet-ui`, `mqttPassword` в `public/config.js`.

