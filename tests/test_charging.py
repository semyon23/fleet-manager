"""Батарея и зарядка: startCharging / stopCharging (VDA 5050), powerSupply в state, batteryCharging в factsheet.

Нужны startCharging и stopCharging с областями INSTANT и NODE в factsheet: без них тесты зарядки
SKIPPED. Тесты батареи (расход, начальный заряд) работают с любым роботом.

Правила, которые проверяются:
  - startCharging (HARD) FINISHED, когда зарядка НАЧАЛАСЬ (powerSupply.charging = true), а не когда
    батарея заполнилась; order со startCharging на последнем узле на этом завершается;
  - после этого робот стоит и заряжается дальше, stateOfCharge растёт;
  - stopCharging выключает зарядку;
  - робот поехал — зарядка прекращается: ни в одном state нет driving и charging одновременно;
  - в движении stateOfCharge уменьшается.

Время "стыковки" startCharging (battery.charging_start_time в robots.yaml) — VDA_CHARGING_START_TIME,
по умолчанию 2 с. Для теста роста заряда initial_state.battery_soc должен быть меньше 100.
"""

from __future__ import annotations

import os
import time

import pytest

from order_builder import Route, build_order, new_order_id
from action_helpers import (
    action, send_instant, wait_instant, wait_order_action, assert_holds,
    assert_stands_while_running, describe,
)

START = "startCharging"
STOP = "stopCharging"
CHARGING_START = float(os.environ.get("VDA_CHARGING_START_TIME", "2.0"))


def charging(s: dict) -> bool:
    return bool(s["powerSupply"]["charging"])


def soc(s: dict) -> float:
    return float(s["powerSupply"]["stateOfCharge"])


def factsheet(fms) -> dict:
    return fms.latest("factsheet") or fms.wait_message("factsheet", lambda m: True, 3.0)


@pytest.fixture
def charging_actions(fms) -> dict:
    """Описания startCharging / stopCharging из factsheet; нет с INSTANT и NODE — тест пропускается."""
    declared = {a["actionType"]: a for a in factsheet(fms)["protocolFeatures"]["mobileRobotActions"]}
    for t in (START, STOP):
        scopes = set(declared.get(t, {}).get("actionScopes", []))
        if not {"INSTANT", "NODE"} <= scopes:
            pytest.skip(f"robot does not declare {t} with INSTANT and NODE scope in factsheet")
    return {t: declared[t] for t in (START, STOP)}


@pytest.fixture(autouse=True)
def stop_charging_after(helper):
    """Тест, упавший на середине, не должен оставить робота на зарядке."""
    yield
    if charging(helper.state()):
        stop_charging(helper)


# ------------------------------------------------------------------ помощники

def request_state(robot) -> dict:
    """Свежий state: робот может не присылать state, пока стоит (только раз в 30 с)."""
    a = action("stateRequest")
    mark = send_instant(robot, a)
    return wait_instant(robot, a, "FINISHED", mark)


def start_charging(robot) -> dict:
    a = action(START, blocking="HARD")
    mark = send_instant(robot, a)
    s = wait_instant(robot, a, "FINISHED", mark, timeout=robot.t(CHARGING_START + 3.0))
    assert charging(s), f"startCharging FINISHED, but powerSupply.charging is false\n  {describe(s)}"
    return s


def stop_charging(robot) -> dict:
    a = action(STOP, blocking="HARD")
    mark = send_instant(robot, a)
    s = wait_instant(robot, a, "FINISHED", mark)
    assert not charging(s), f"stopCharging FINISHED, but powerSupply.charging is true\n  {describe(s)}"
    return s


def straight_order(robot, step: float, last_node_actions: list[dict] | None = None):
    route = Route(robot.pose(), count=1, step=step)
    order = build_order(new_order_id(), 0, route.nodes, released=2, map_id=route.map_id)
    if last_node_actions:
        order["nodes"][-1]["actions"] = last_node_actions
    return route, order


def send_accepted(robot, order) -> int:
    mark = robot.send(order)
    robot.wait_accepted(order, mark)
    return mark


# ------------------------------------------------------------------ factsheet

def test_factsheet_declares_charging(fms, charging_actions):
    for t, a in charging_actions.items():
        assert "HARD" in a.get("blockingTypes", []), f"{t}: VDA requires blockingType HARD"

    bc = (factsheet(fms).get("mobileRobotConfiguration") or {}).get("batteryCharging")
    assert bc, "factsheet has no mobileRobotConfiguration.batteryCharging"
    levels = [bc.get("criticalLowChargingLevel"), bc.get("minimumDesiredChargingLevel"),
              bc.get("maximumDesiredChargingLevel")]
    assert None not in levels, f"batteryCharging levels are incomplete: {bc}"
    assert 0.0 <= levels[0] <= levels[1] <= levels[2] <= 100.0, \
        f"expected 0 <= critical <= minimumDesired <= maximumDesired <= 100, got {levels}"


# ------------------------------------------------------------------ батарея

def test_power_supply_is_initialised(robot):
    s = request_state(robot)
    assert 0.0 < soc(s) <= 100.0, \
        f"stateOfCharge = {soc(s)}: power supply not initialised (initial_state.battery_soc)"
    assert not charging(s)


def test_state_of_charge_drains_while_driving(robot):
    before = soc(request_state(robot))
    assert before > 0.0, "battery is empty, nothing to drain"
    _, order = straight_order(robot, step=3.0)
    send_accepted(robot, order)
    robot.wait_idle(robot.drive_timeout(3.0))
    after = soc(request_state(robot))
    assert after < before, f"stateOfCharge did not decrease while driving: {before} -> {after}"


# ------------------------------------------------------------------ зарядка order'ом

def test_order_to_charger_finishes_when_charging_starts(robot, charging_actions):
    a = action(START, blocking="HARD")
    route, order = straight_order(robot, step=2.0, last_node_actions=[a])
    mark = send_accepted(robot, order)

    s = wait_order_action(robot, a, "RUNNING", mark, robot.drive_timeout(2.0))
    assert s["lastNodeId"] == route.node_id(1), f"startCharging must run at the charger node\n  {describe(s)}"
    assert not charging(s), "charging must start only when startCharging finishes (docking phase)"

    s = wait_order_action(robot, a, "FINISHED", mark, robot.t(CHARGING_START + 3.0))
    assert charging(s), f"startCharging FINISHED, but powerSupply.charging is false\n  {describe(s)}"

    # Order завершён сразу после начала зарядки, а не когда батарея заполнилась.
    s = robot.wait_idle(robot.t(3.0))
    assert s["lastNodeId"] == route.node_id(1)
    assert charging(s)
    assert_stands_while_running(robot, a, mark)

    assert_holds(robot, 1.5, lambda x: charging(x) and not x["driving"],
                 "after the order the robot must stand and keep charging")


def test_state_of_charge_grows_while_charging(robot, charging_actions):
    if soc(request_state(robot)) >= 99.5:
        pytest.skip("battery is full: set initial_state.battery_soc below 100 in robots.yaml")
    start_charging(robot)
    s1 = request_state(robot)
    time.sleep(robot.t(2.0))
    s2 = request_state(robot)
    assert charging(s2)
    assert soc(s2) > soc(s1), f"stateOfCharge did not grow while charging: {soc(s1)} -> {soc(s2)}"
    assert soc(s2) <= 100.0


# ------------------------------------------------------------------ instant'ы

def test_instant_start_and_stop_charging(robot, charging_actions):
    start_charging(robot)
    assert_holds(robot, 1.0, charging, "charging must stay on until stopCharging")

    stop_charging(robot)
    assert_holds(robot, 1.0, lambda s: not charging(s), "charging must stay off after stopCharging")


def test_start_charging_when_already_charging(robot, charging_actions):
    start_charging(robot)
    a = action(START, blocking="HARD")
    mark = send_instant(robot, a)
    # Уже заряжается — без повторной "стыковки": FINISHED быстрее, чем charging_start_time.
    s = wait_instant(robot, a, "FINISHED", mark, timeout=robot.t(max(1.0, CHARGING_START * 0.75)))
    assert charging(s)


def test_stop_charging_when_not_charging(robot, charging_actions):
    s = stop_charging(robot)
    assert not s["driving"]


# ------------------------------------------------------------------ движение прекращает зарядку

def test_driving_stops_charging(robot, charging_actions):
    start_charging(robot)
    _, order = straight_order(robot, step=1.5)
    mark = send_accepted(robot, order)

    s = robot.fms.wait_state(lambda x: x["driving"], robot.t(5.0), since=mark, what="driving")
    assert not charging(s), f"robot drives while charging\n  {describe(s)}"
    robot.wait_idle(robot.drive_timeout(1.5))

    for x in robot.fms.states(mark):
        assert not (x["driving"] and charging(x)), f"robot drives while charging\n  {describe(x)}"
    assert not charging(request_state(robot)), "charging must not resume by itself after the order"
