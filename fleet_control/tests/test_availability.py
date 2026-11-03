"""Готовность робота (C-4b): control считает availability по статусу от dispatch и пишет её в базу.

Проверяем то, что увидит UI, — поле availability в GET базы. Тесты с sim_robot без симулятора пропускаются.
"""

from __future__ import annotations

import time

import fleet_dispatch_pb2 as dpb
import fms_types_pb2 as fms
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


def put_on_graph(robots, dispatch, robot_id: str) -> None:
    dispatch.InitializePosition(dpb.InitializePositionRequest(
        robot_id=robot_id, pose=fms.Pose(x=0, y=0, theta=0, map_id=SIM_MAP), last_node_id="origin"))
    robots.wait_status(robot_id, lambda s: s["lastNodeId"] == "origin", 5, "robot on node origin")


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

def test_robot_on_graph_is_idle(robots, sim_robot, dispatch):
    put_on_graph(robots, dispatch, sim_robot)
    availability = wait_availability(robots, sim_robot, "IDLE")
    assert availability["reasons"] == []


def test_paused_robot(robots, sim_robot, dispatch):
    put_on_graph(robots, dispatch, sim_robot)
    wait_availability(robots, sim_robot, "IDLE")

    robots.command(sim_robot, "pause")
    wait_availability(robots, sim_robot, "PAUSED")
    robots.command(sim_robot, "resume")
    wait_availability(robots, sim_robot, "IDLE")


def test_robot_with_mission_is_busy(robots, sim_robot, dispatch):
    start_mission(robots, dispatch, sim_robot)
    wait_availability(robots, sim_robot, "BUSY")
