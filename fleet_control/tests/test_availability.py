"""Готовность робота (C-4b): control считает availability по статусу от dispatch и пишет её в базу.

Проверяем то, что увидит UI, — поле availability в GET базы. Тесты с sim_robot без симулятора пропускаются.
"""

from __future__ import annotations

import time

from conftest import SIM_MAP
from test_robot_commands import start_mission


def wait_availability(robots, robot_id: str, state: str, timeout: float = 5) -> dict:
    deadline = time.monotonic() + timeout
    availability = None
    while time.monotonic() < deadline:
        code, robot = robots.from_database(robot_id)
        availability = robot.get("availability") if code == 200 else None
        if availability and availability["state"] == state:
            return availability
        time.sleep(0.2)
    raise TimeoutError(f"timeout {timeout:.0f}s waiting for availability {state}\nlast: {availability}")


def put_on_graph(robots, robot_id: str, sim_map: dict) -> str:
    """M-2: на графе — на узле карты робота, которая есть в fms. Ставим через control, как оператор в UI."""
    node_id = sim_map["waypoints"][0]["id"]
    robots.place(robot_id, {"mapId": SIM_MAP, "nodeId": node_id}, lambda s: s["lastNodeId"] == node_id)
    return node_id


# ------------------------------------------------------------------ без симулятора

def test_robot_that_never_connected_is_offline(robots):
    """Выдуманный робот: dispatch подписался, но от робота ничего нет."""
    _, robot = robots.register()
    availability = wait_availability(robots, robot["id"], "OFFLINE")
    assert availability["reasons"], "OFFLINE should say why"
    assert all(set(reason) == {"code", "message"} for reason in availability["reasons"])


def test_availability_is_written_right_after_registration(robots):
    """По контракту availability есть у каждого робота — null она только до первой записи control."""
    _, robot = robots.register()
    wait_availability(robots, robot["id"], "OFFLINE", timeout=2)


# ------------------------------------------------------------------ с роботом симулятора

def test_robot_on_graph_is_idle(sim_map, robots, sim_robot):
    put_on_graph(robots, sim_robot, sim_map)
    availability = wait_availability(robots, sim_robot, "IDLE")
    assert availability["reasons"] == []


def test_paused_robot(sim_map, robots, sim_robot):
    put_on_graph(robots, sim_robot, sim_map)
    wait_availability(robots, sim_robot, "IDLE")

    robots.command(sim_robot, "pause")
    wait_availability(robots, sim_robot, "PAUSED")
    robots.command(sim_robot, "resume")
    wait_availability(robots, sim_robot, "IDLE")


def test_robot_with_mission_is_busy(robots, sim_robot, dispatch):
    start_mission(robots, dispatch, sim_robot)
    wait_availability(robots, sim_robot, "BUSY")
