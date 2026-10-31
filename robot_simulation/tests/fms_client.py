"""Мини-fms для тестов: MQTT-клиент, который отправляет роботу order'а
и собирает всё, что робот публикует (state, connection, factsheet, visualization).

Все входящие сообщения складываются в журнал. Тесты ждут нужное состояние
через wait_state(): условие проверяется только для сообщений, пришедших
после отметки mark(), поэтому старые state не дают ложных срабатываний.
"""

from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Callable, Optional

import paho.mqtt.client as mqtt


@dataclass
class RobotTarget:
    host: str
    port: int
    interface: str
    major: str
    manufacturer: str
    serial: str
    username: Optional[str] = None
    password: Optional[str] = None

    @property
    def base_topic(self) -> str:
        return f"{self.interface}/{self.major}/{self.manufacturer}/{self.serial}"

    def topic(self, name: str) -> str:
        return f"{self.base_topic}/{name}"


@dataclass
class Received:
    topic: str
    payload: Any          # dict, если JSON разобрался, иначе исходная строка
    retained: bool
    qos: int
    time: float = field(default_factory=time.monotonic)


def utc_timestamp() -> str:
    now = datetime.now(timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"


class FmsClient:
    def __init__(self, target: RobotTarget, client_id: str = "vda-robot-tests"):
        self.target = target
        self._lock = threading.Condition()
        self._log: dict[str, list[Received]] = {}
        self._header_ids: dict[str, int] = {}
        self._listeners: list[Callable[[str, Received], None]] = []
        # Вся переписка с роботом: (время, "->" или "<-", топик, текст) — для логов упавших тестов.
        self._traffic: list[tuple[float, str, str, str]] = []

        self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        if target.username:
            self._client.username_pw_set(target.username, target.password)
        self._client.on_message = self._on_message
        self._connected = threading.Event()
        self._client.on_connect = lambda *_: self._connected.set()

    # ------------------------------------------------------------------ связь

    def connect(self, timeout: float = 5.0) -> None:
        self._client.connect(self.target.host, self.target.port, keepalive=30)
        self._client.loop_start()
        if not self._connected.wait(timeout):
            raise TimeoutError(f"no connection to broker {self.target.host}:{self.target.port}")
        for name in ("state", "connection", "factsheet", "visualization"):
            self._client.subscribe(self.target.topic(name), qos=1)

    def close(self) -> None:
        self._client.loop_stop()
        self._client.disconnect()

    def add_listener(self, fn: Callable[[str, Received], None]) -> None:
        self._listeners.append(fn)

    def _on_message(self, _client, _userdata, msg) -> None:
        name = msg.topic.rsplit("/", 1)[-1]
        try:
            payload: Any = json.loads(msg.payload)
        except ValueError:
            payload = msg.payload.decode(errors="replace")
        rec = Received(msg.topic, payload, bool(msg.retain), msg.qos)
        with self._lock:
            self._log.setdefault(name, []).append(rec)
            self._traffic.append((rec.time, "<-", name, msg.payload.decode(errors="replace")))
            self._lock.notify_all()
        for fn in self._listeners:
            fn(name, rec)

    # ------------------------------------------------------------- отправка

    def _header(self, topic_name: str) -> dict:
        self._header_ids[topic_name] = self._header_ids.get(topic_name, 0) + 1
        return {
            "headerId": self._header_ids[topic_name],
            "timestamp": utc_timestamp(),
            "version": "3.0.0",
            "manufacturer": self.target.manufacturer,
            "serialNumber": self.target.serial,
        }

    def send_order(self, order: dict) -> dict:
        """Добавляет заголовок и отправляет. Возвращает отправленное сообщение."""
        msg = {**self._header("order"), **order}
        self.send_raw("order", json.dumps(msg))
        return msg

    def send_raw(self, topic_name: str, payload: str) -> None:
        with self._lock:
            self._traffic.append((time.monotonic(), "->", topic_name, payload))
        self._client.publish(self.target.topic(topic_name), payload, qos=0).wait_for_publish(2)

    def traffic(self, since: float) -> list[tuple[float, str, str, str]]:
        with self._lock:
            return [t for t in self._traffic if t[0] >= since]

    # --------------------------------------------------------------- чтение

    def mark(self, name: str = "state") -> int:
        with self._lock:
            return len(self._log.get(name, []))

    def messages(self, name: str, since: int = 0) -> list[Received]:
        with self._lock:
            return list(self._log.get(name, [])[since:])

    def latest(self, name: str = "state") -> Optional[dict]:
        with self._lock:
            items = self._log.get(name, [])
            return items[-1].payload if items else None

    def wait_message(self, name: str, predicate: Callable[[Any], bool], timeout: float,
                     since: int = 0, what: str = "") -> Any:
        deadline = time.monotonic() + timeout
        index = since
        with self._lock:
            while True:
                items = self._log.get(name, [])
                while index < len(items):
                    payload = items[index].payload
                    index += 1
                    if isinstance(payload, dict) and predicate(payload):
                        return payload
                left = deadline - time.monotonic()
                if left <= 0:
                    last = items[-1].payload if items else None
                    raise TimeoutError(
                        f"timeout {timeout:.1f}s waiting for {name}: {what or predicate}\n"
                        f"last {name}: {json.dumps(last, indent=1)[:1500] if last else None}")
                self._lock.wait(left)

    def wait_state(self, predicate: Callable[[dict], bool], timeout: float,
                   since: Optional[int] = None, what: str = "") -> dict:
        return self.wait_message("state", predicate,
                                 timeout, self.mark() if since is None else since, what)

    def states(self, since: int = 0) -> list[dict]:
        return [r.payload for r in self.messages("state", since) if isinstance(r.payload, dict)]


# ---------------------------------------------------------------- помощники по state

def is_idle(state: dict) -> bool:
    """6.6.8: nodeStates и edgeStates пусты, все action'ы FINISHED или FAILED."""
    return (not state.get("nodeStates") and not state.get("edgeStates") and
            all(a.get("actionStatus") in ("FINISHED", "FAILED")
                for a in state.get("actionStates", [])))


def error_types(state: dict) -> set[str]:
    return {e.get("errorType") for e in state.get("errors", [])}


def has_error(state: dict, error_type: str) -> bool:
    return error_type in error_types(state)


ORDER_REJECTION_ERRORS = {
    "VALIDATION_FAILURE", "UNSUPPORTED_PARAMETER", "INVALID_ORDER_ACTION",
    "OUTDATED_ORDER_UPDATE", "SAME_ORDER_UPDATE_ID", "ORDER_UPDATE_FOLLOWING_CANCEL",
    "OTHER_ORDER_ACTIVE", "START_NODE_OUT_OF_RANGE", "NO_ROUTE_TO_TARGET",
    "MOBILE_ROBOT_NOT_AVAILABLE", "UNKNOWN_MAP_ID",
}


def rejection_errors(state: dict) -> set[str]:
    return error_types(state) & ORDER_REJECTION_ERRORS
