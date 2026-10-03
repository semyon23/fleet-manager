"""Правила, которые должны выполняться для КАЖДОГО state, независимо от сценария.

Проверка подключается к клиенту как слушатель и копит нарушения.
После каждого теста conftest.py проверяет, что нарушений нет.
"""

from __future__ import annotations

import re
import threading
from typing import Optional

from fms_client import Received

TIMESTAMP_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,9})?Z$")

STATE_REQUIRED = [
    "headerId", "timestamp", "version", "manufacturer", "serialNumber",
    "orderId", "orderUpdateId", "lastNodeId", "lastNodeSequenceId",
    "nodeStates", "edgeStates", "driving", "actionStates",
    "powerSupply", "operatingMode", "errors", "safetyState",
]
OPERATING_MODES = {"STARTUP", "AUTOMATIC", "SEMIAUTOMATIC", "INTERVENED", "MANUAL",
                   "SERVICE", "TEACH_IN"}
ACTION_STATUSES = {"WAITING", "INITIALIZING", "RUNNING", "PAUSED", "RETRIABLE",
                   "FINISHED", "FAILED"}


class InvariantChecker:
    def __init__(self, manufacturer: str, serial: str, max_state_interval: float):
        self.manufacturer = manufacturer
        self.serial = serial
        self.max_state_interval = max_state_interval
        self._lock = threading.Lock()
        self.violations: list[str] = []
        self._last_header: dict[str, int] = {}
        self._prev_state: Optional[dict] = None
        self._prev_state_time: Optional[float] = None

    def take_violations(self) -> list[str]:
        with self._lock:
            v, self.violations = self.violations, []
            return v

    def _fail(self, msg: str) -> None:
        self.violations.append(msg)

    def __call__(self, name: str, rec: Received) -> None:
        if rec.retained:  # retained-сообщение могло остаться от прошлого запуска робота
            return
        with self._lock:
            msg = rec.payload
            if not isinstance(msg, dict):
                self._fail(f"{name}: payload is not JSON")
                return
            self._check_header(name, msg)
            if name == "state":
                self._check_state(msg, rec.time)

    # ----------------------------------------------------------------- header

    def _check_header(self, name: str, msg: dict) -> None:
        if msg.get("manufacturer") != self.manufacturer or msg.get("serialNumber") != self.serial:
            self._fail(f"{name}: manufacturer/serialNumber {msg.get('manufacturer')}/"
                       f"{msg.get('serialNumber')} do not match topic")
        if not str(msg.get("version", "")).startswith("3."):
            self._fail(f"{name}: version '{msg.get('version')}' is not 3.x")
        if not TIMESTAMP_RE.match(str(msg.get("timestamp", ""))):
            self._fail(f"{name}: bad timestamp '{msg.get('timestamp')}'")
        hid = msg.get("headerId")
        prev = self._last_header.get(name)
        # connection: last will публикует брокер со старым headerId (6.5) — не проверяем
        if name != "connection" and prev is not None and isinstance(hid, int) and hid != prev + 1:
            self._fail(f"{name}: headerId {prev} -> {hid} (must grow by 1 per topic)")
        if isinstance(hid, int):
            self._last_header[name] = hid

    # ------------------------------------------------------------------ state

    def _check_state(self, s: dict, t: float) -> None:
        missing = [k for k in STATE_REQUIRED if k not in s]
        if missing:
            self._fail(f"state: missing fields {missing}")
            return
        if s["operatingMode"] not in OPERATING_MODES:
            self._fail(f"state: unknown operatingMode {s['operatingMode']}")

        last_seq = s["lastNodeSequenceId"]
        has_last = bool(s["lastNodeId"])

        # nodeStates: чётные, по возрастанию, впереди робота; released — сплошным префиксом
        seqs = [n.get("sequenceId") for n in s["nodeStates"]]
        if seqs != sorted(seqs) or len(set(seqs)) != len(seqs):
            self._fail(f"state: nodeStates not strictly ordered: {seqs}")
        if any(q % 2 for q in seqs):
            self._fail(f"state: odd sequenceId in nodeStates: {seqs}")
        if has_last and any(q <= last_seq for q in seqs):
            self._fail(f"state: nodeStates contain passed nodes (lastNodeSequenceId={last_seq}): {seqs}")
        released = [bool(n.get("released")) for n in s["nodeStates"]]
        first_horizon = next((i for i, r in enumerate(released) if not r), len(released))
        if any(released[first_horizon:]):
            self._fail(f"state: released node after horizon in nodeStates: {released}")

        eseqs = [e.get("sequenceId") for e in s["edgeStates"]]
        if eseqs != sorted(eseqs) or any(q % 2 == 0 for q in eseqs):
            self._fail(f"state: bad edgeStates sequenceIds: {eseqs}")
        if has_last and any(q < last_seq for q in eseqs):
            self._fail(f"state: edgeStates contain passed edges (lastNodeSequenceId={last_seq}): {eseqs}")

        for a in s["actionStates"] + s.get("instantActionStates", []):
            if a.get("actionStatus") not in ACTION_STATUSES:
                self._fail(f"state: bad actionStatus {a.get('actionStatus')}")

        # Позиция: lastNodeSequenceId не уменьшается в пределах одного order'а
        p = self._prev_state
        if p is not None and p.get("orderId") == s["orderId"] and has_last \
                and p.get("lastNodeId") and last_seq < p.get("lastNodeSequenceId", 0):
            self._fail(f"state: lastNodeSequenceId went back {p['lastNodeSequenceId']} -> {last_seq}")

        # 6.6: state не реже раза в max_state_interval (+ запас на доставку)
        if self._prev_state_time is not None and t - self._prev_state_time > self.max_state_interval + 2.0:
            self._fail(f"state: gap {t - self._prev_state_time:.1f}s > {self.max_state_interval}s")

        self._prev_state = s
        self._prev_state_time = t
