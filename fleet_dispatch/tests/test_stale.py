"""STALE: робот на связи с брокером, но state не присылает.

Медленный (около полутора минут), включается DISPATCH_RUN_SLOW=1. Замораживает процесс симулятора
сигналом SIGSTOP, поэтому тесты должны работать на той же машине, что и симулятор.
"""

from __future__ import annotations

import os
import signal
import subprocess

import pytest

import fms_types_pb2 as fms
from dispatch_client import robot_status

pytestmark = pytest.mark.skipif(os.environ.get("DISPATCH_RUN_SLOW") != "1",
                                reason="slow test, enable with DISPATCH_RUN_SLOW=1")


def simulator_pid() -> int:
    result = subprocess.run(["pgrep", "-f", "robot_simulation"], capture_output=True, text=True)
    pids = [int(p) for p in result.stdout.split()]
    if len(pids) != 1:
        pytest.skip(f"expected exactly one robot_simulation process on this machine, found {len(pids)}")
    return pids[0]


def test_frozen_robot_becomes_stale_and_recovers(robot):
    pid = simulator_pid()
    updates = robot.robot_updates()
    updates.wait(lambda u: u.HasField("snapshot"), robot.settings.t(3), what="snapshot")

    os.kill(pid, signal.SIGSTOP)   # MQTT-соединение живо, но state не приходит
    try:
        # 60 с без state + до 5 с на тик сторожа; последний state мог прийти незадолго до заморозки
        updates.wait(robot_status(robot.robot_id, lambda s: s.connection == fms.CONNECTION_STATE_STALE),
                     robot.settings.t(75), what="STALE")
    finally:
        os.kill(pid, signal.SIGCONT)

    robot.request_state()
    updates.wait(robot_status(robot.robot_id, lambda s: s.connection == fms.CONNECTION_STATE_ONLINE),
                 robot.settings.t(10), what="ONLINE again")
