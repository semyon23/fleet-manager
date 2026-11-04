#!/usr/bin/env bash
# Запуск fms на стенде одной командой: база, dispatch, control, симулятор — в фоне, логи в $LOG_DIR.
# PostgreSQL, mosquitto и nginx — системные службы, скрипт их только проверяет (deploy/README.md).
# Ctrl+C — остановить всё. Упал один сервис — останавливаются и остальные.
#
#   deploy/run-stand.sh
#   BUILD_DIR=cmake-build-release deploy/run-stand.sh
#
# Логи: tail -f ~/fms_logs/*.log
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/cmake-build-debug}"
[[ "$BUILD" = /* ]] || BUILD="$ROOT/$BUILD"
LOGS="${LOG_DIR:-$HOME/fms_logs}"
export MQTT_PORT="${MQTT_PORT:-1884}"   # на стенде брокер на 1884 (deploy/README.md, раздел про mosquitto)

# имя (оно же имя лога), бинарник, порт готовности («-» — не ждать)
SERVICES=(
  "database  fleet_database/fleet_database      5002"
  "dispatch  fleet_dispatch/fleet_dispatch      5001"
  "control   fleet_control/fleet_control        8003"
  "simulator robot_simulation/robot_simulation  -"
)

for unit in postgresql mosquitto nginx; do
  systemctl is-active --quiet "$unit" || { echo "$unit не запущен: sudo systemctl start $unit"; exit 1; }
done

for line in "${SERVICES[@]}"; do
  read -r name bin port <<<"$line"
  [[ -x "$BUILD/$bin" ]] || { echo "нет $BUILD/$bin — собрать проект или задать BUILD_DIR"; exit 1; }
  if pgrep -f "^[^ ]*$bin( |$)" >/dev/null; then echo "$name уже запущен: pgrep -af $bin"; exit 1; fi
done

mkdir -p "$LOGS"
PIDS=()
NAMES=()

stop_all() {
  trap - INT TERM EXIT
  echo "останавливаю..."
  for ((i = ${#PIDS[@]} - 1; i >= 0; i--)); do kill "${PIDS[i]}" 2>/dev/null; done
  wait
  echo "fms остановлена"
  exit
}
trap stop_all INT TERM EXIT

wait_port() {
  for _ in $(seq 50); do
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
    sleep 0.2
  done
  return 1
}

for line in "${SERVICES[@]}"; do
  read -r name bin port <<<"$line"
  echo "=== start $(date '+%F %T')" >>"$LOGS/$name.log"
  (cd "$ROOT" && exec "$BUILD/$bin") >>"$LOGS/$name.log" 2>&1 &
  PIDS+=($!)
  NAMES+=("$name")
  if [[ "$port" != "-" ]] && ! wait_port "$port"; then
    echo "$name не открыл порт $port за 10 с — смотри $LOGS/$name.log"
    exit 1
  fi
  echo "$name запущен (pid $!), лог $LOGS/$name.log"
done

curl -s --max-time 3 -w '\n' http://127.0.0.1:8080/api/health || echo "шлюз не ответил: sudo systemctl status nginx"
echo "UI: http://localhost:8080/   Ctrl+C — остановить"

wait -n
for i in "${!PIDS[@]}"; do
  kill -0 "${PIDS[i]}" 2>/dev/null || echo "${NAMES[i]} завершился — смотри $LOGS/${NAMES[i]}.log"
done
