# Развёртывание fms

## Стенд одной командой

Когда nginx, mosquitto и PostgreSQL уже настроены (разделы ниже), а проект собран, [`run-stand.sh`](run-stand.sh)
запускает базу, dispatch, control и симулятор — по порядку, каждый ждёт, пока предыдущий откроет порт:

```bash
deploy/run-stand.sh
```

- логи — `~/fms_logs/<сервис>.log` (`LOG_DIR`), смотреть: `tail -f ~/fms_logs/*.log`;
- сборка — `cmake-build-debug` (`BUILD_DIR`), брокер — порт 1884 (`MQTT_PORT`);
- Ctrl+C останавливает всё; упал один сервис — останавливаются и остальные, скрипт говорит, какой;
- службы не запущены или сервис уже работает (запущен руками) — скрипт скажет и ничего не запустит.

## Шлюз nginx

Всё, что нужно браузеру, — с одного адреса `http://<машина>:8080/`. Шлюз [`nginx/fms.conf`](nginx/fms.conf)
раздаёт UI и разводит запросы так, как требует контракт UI (раздел 1):

| Запрос | Куда |
|---|---|
| `/` | редирект на `/fleet-manager/` |
| `/fleet-manager/*` | собранный UI из `/opt/fms/www/fleet-manager` |
| `GET /api/fms/*` | fleet_database, `127.0.0.1:8002` — только чтение |
| остальное `/api/*` | fleet_control, `127.0.0.1:8003` — действия и `/api/health` |
| `/mqtt` | mosquitto, WebSocket-listener `127.0.0.1:9001` — живая поза роботов |

UI и API с одного адреса — CORS браузеру не нужен, а наружу открыт один порт 8080.

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

Сейчас в шлюзе нет проверки JWT: открывать порт 8080 наружу нельзя, только в локальной сети стенда.
Позже — проверка JWT и Docker-образы сервисов.

### Веб-UI

UI собирается в его репозитории (`npm ci && npm run build`, нужен Node.js 20+) — где угодно, хоть на другой
машине. Результат — папка `dist/`, её содержимое кладётся в `/opt/fms/www/fleet-manager`:

```bash
sudo mkdir -p /opt/fms/www/fleet-manager && sudo cp -r <путь к dist>/. /opt/fms/www/fleet-manager/
```

Поверх `config.js` из сборки — конфиг для шлюза [`ui/config.js`](ui/config.js): адрес API и брокера он берёт
из адреса страницы, править под машину не нужно.

```bash
sudo cp deploy/ui/config.js /opt/fms/www/fleet-manager/config.js
```

В нём вписать пароль пользователя брокера `fleet-ui` (`mqttPassword`):

```bash
sudo nano /opt/fms/www/fleet-manager/config.js
```

Проверка — UI отдаётся, глубокая ссылка тоже открывает UI:

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/fleet-manager/robots
```

Ответ `200`. В браузере — `http://<машина>:8080/`; в шапке UI индикатор MQTT зелёный, роботы двигаются на Live Map.
Новая сборка UI — снова скопировать `dist/` и `config.js`, перезапускать nginx не нужно.

**WSL.** Порты внутри WSL с других машин не видны. Либо пробросить 8080 на Windows (PowerShell от администратора):

```powershell
netsh interface portproxy add v4tov4 listenport=8080 listenaddress=0.0.0.0 connectport=8080 connectaddress=<IP WSL>
```

```powershell
New-NetFirewallRule -DisplayName "fms 8080" -Direction Inbound -Protocol TCP -LocalPort 8080 -Action Allow
```

(`<IP WSL>` — `hostname -I` внутри WSL; он меняется после перезапуска WSL), либо с ноутбука — SSH-туннель
на один порт и UI по `http://localhost:8080/`:

```bash
ssh -N -L 8080:127.0.0.1:8080 -p <порт SSH> <пользователь>@<машина>
```

## MQTT-брокер (mosquitto)

Конфиг [`mosquitto/fms.conf`](mosquitto/fms.conf) и права [`mosquitto/fms.acl`](mosquitto/fms.acl):

| Порт | Кто | Протокол |
|---|---|---|
| `1883` | роботы, fleet_dispatch, симулятор | MQTT |
| `9001` | веб-UI через шлюз (`/mqtt`), только `127.0.0.1` | MQTT over WebSocket |

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

Конфиг fms заменяет основной `mosquitto.conf` целиком (рядом со стандартным из `conf.d` он не запустится:
`Duplicate persistence_location`). Старый — в копию:

```bash
sudo cp /etc/mosquitto/mosquitto.conf /etc/mosquitto/mosquitto.conf.orig
```

```bash
sudo cp deploy/mosquitto/fms.conf /etc/mosquitto/mosquitto.conf && sudo cp deploy/mosquitto/fms.acl /etc/mosquitto/fms.acl
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

На стенде, где порт 1883 занят (WSL с `netsh portproxy`), в `/etc/mosquitto/mosquitto.conf` поставить `listener 1884`.

**Стенд без паролей** — для тестов, только в закрытой сети: вход без логина, любой клиент читает и пишет любые топики
(логины в настройках клиентов можно оставить — без файла паролей брокер их не проверяет). Пользователей тогда не
создавать, а в конфиге включить анонимов и выключить пароли и права:

```bash
sudo sed -i 's/^allow_anonymous false/allow_anonymous true/; s/^password_file/#password_file/; s/^acl_file/#acl_file/' /etc/mosquitto/mosquitto.conf
```

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
- **веб-UI** — ничего, кроме пароля: при раздаче через шлюз `config.js` из [`ui/config.js`](ui/config.js)
  (`mqttUrl` = `ws://<машина>:8080/mqtt`, `mqttUser` = `fleet-ui`), `mqttPassword` вписать на месте.

