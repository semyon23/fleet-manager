"""Настройка тестов. Всё задаётся переменными окружения (см. README.md)."""

from __future__ import annotations

import json
import os
import re
import time
import uuid
from pathlib import Path

import pytest

from fms_client import FmsClient, RobotTarget
from invariants import InvariantChecker
from robot_helper import RobotHelper


def env(name: str, default: str | None = None) -> str | None:
    return os.environ.get(name, default)


@pytest.fixture(scope="session")
def target() -> RobotTarget:
    return RobotTarget(
        host=env("VDA_BROKER_HOST", "localhost"),
        port=int(env("VDA_BROKER_PORT", "1883")),
        interface=env("VDA_INTERFACE", "amr"),
        major=env("VDA_MAJOR", "v3"),
        manufacturer=env("VDA_MANUFACTURER", "KTC"),
        serial=env("VDA_SERIAL", "amr1"),
        username=env("VDA_USER"),
        password=env("VDA_PASSWORD"),
    )


@pytest.fixture(scope="session")
def invariants() -> InvariantChecker:
    return InvariantChecker(env("VDA_MANUFACTURER", "KTC"), env("VDA_SERIAL", "amr1"),
                            max_state_interval=float(env("VDA_MAX_STATE_INTERVAL", "30")))


@pytest.fixture(scope="session")
def fms(target: RobotTarget, invariants: InvariantChecker):
    client = FmsClient(target, client_id=f"vda-robot-tests-{uuid.uuid4().hex[:6]}")
    client.add_listener(invariants)
    client.connect()
    # Первый state: робот шлёт его при подключении и не реже раза в 30 с.
    try:
        client.wait_message("state", lambda s: True, timeout=35.0, what="first state")
    except TimeoutError:
        pytest.exit(f"no state from robot on '{target.topic('state')}' — is the simulator running "
                    f"and are VDA_INTERFACE / VDA_MANUFACTURER / VDA_SERIAL correct?", returncode=2)
    yield client
    client.close()


@pytest.fixture(scope="session")
def helper(fms: FmsClient) -> RobotHelper:
    return RobotHelper(fms, timeout_scale=float(env("VDA_TIMEOUT_SCALE", "1.0")),
                       seconds_per_meter=float(env("VDA_SECONDS_PER_METER", "4.0")))


@pytest.fixture
def robot(helper: RobotHelper) -> RobotHelper:
    """Перед тестом: робот в AUTOMATIC, idle, стоит, без ошибок отказа order'а."""
    s = helper.state()
    assert s["operatingMode"] in ("AUTOMATIC", "SEMIAUTOMATIC"), \
        f"robot operatingMode is {s['operatingMode']}, orders will be rejected"
    assert s.get("mobileRobotPosition", {}).get("localized"), "robot is not localized"
    helper.finish_waiting_order()
    try:
        helper.wait_idle(helper.drive_timeout(10))
        helper.clear_order_errors()
    except TimeoutError as e:
        # Робот застрял после предыдущего теста: иначе каждый следующий тест
        # выжидал бы полный таймаут. Останавливаем прогон с понятной причиной.
        pytest.exit(f"robot cannot return to idle, remaining tests aborted:\n{e}", returncode=3)
    return helper


@pytest.fixture(autouse=True)
def check_invariants(invariants: InvariantChecker):
    invariants.take_violations()  # нарушения прошлых тестов уже отчитаны
    yield
    violations = invariants.take_violations()
    assert not violations, "state invariants violated:\n  " + "\n  ".join(violations)


# ------------------------------------------------------------------ логи переписки

@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    """Запоминаем результат каждой фазы теста, чтобы фикстура знала, упал ли он."""
    outcome = yield
    report = outcome.get_result()
    setattr(item, f"rep_{report.when}", report)


def _format_traffic(traffic, start: float, with_visualization: bool) -> str:
    lines = []
    for t, direction, topic, payload in traffic:
        if topic == "visualization" and not with_visualization:
            continue
        try:
            payload = json.dumps(json.loads(payload), ensure_ascii=False, indent=2)
        except ValueError:
            pass  # битый JSON (например, в тесте отказов) пишем как есть
        lines.append(f"[{t - start:8.3f}s] {direction} {topic}\n{payload}\n")
    return "\n".join(lines)


@pytest.fixture(autouse=True)
def traffic_log(request, fms):
    """Упавший тест: вся переписка с роботом за время теста — в logs/<тест>.log.
    VDA_LOG_ALL=1 — писать лог для каждого теста, VDA_LOG_VISUALIZATION=1 — с visualization."""
    start = time.monotonic()
    yield
    failed = any(getattr(getattr(request.node, f"rep_{phase}", None), "failed", False)
                 for phase in ("setup", "call"))
    if not failed and env("VDA_LOG_ALL") != "1":
        return
    log_dir = Path(env("VDA_LOG_DIR", "logs"))
    log_dir.mkdir(parents=True, exist_ok=True)
    name = re.sub(r"[^\w.-]+", "_", request.node.nodeid)
    path = log_dir / f"{name}.log"
    text = _format_traffic(fms.traffic(start), start, env("VDA_LOG_VISUALIZATION") == "1")
    path.write_text(f"{request.node.nodeid}\n-> отправлено роботу, <- получено от робота\n\n{text}",
                    encoding="utf-8")
    print(f"\nMQTT traffic of this test: {path.resolve()}")
