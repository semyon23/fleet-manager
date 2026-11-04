"""Робот на графе (M-2): POST /robots/{id}/position ставит робота на узел карты fms — control берёт координаты
узла из графа и шлёт через dispatch VDA initializePosition. Готовность учитывает карты: карты робота нет
в fms — UNKNOWN_MAP, lastNodeId не узел его карты — NOT_ON_GRAPH.

Тесты с sim_robot без симулятора пропускаются.
"""

from __future__ import annotations

import time

import pytest

import fleet_dispatch_pb2 as dpb
import fms_types_pb2 as fms
from conftest import CONTROL, SIM_MAP, http, node_xy
from test_availability import wait_availability
from test_robot_commands import start_mission

NO_ROBOT = "test-ctl-no-such-robot"


def on_node(node_id: str, theta: float | None = None):
    """Условие для robots.place: робот на узле (и с этой ориентацией)."""
    def applied(status: dict) -> bool:
        pose = status.get("pose")
        return status["lastNodeId"] == node_id and pose is not None and (
            theta is None or pose["theta"] == pytest.approx(theta))
    return applied


def wait_reason(robots, robot_id: str, code: str, timeout: float = 5) -> dict:
    deadline = time.monotonic() + timeout
    availability = None
    while time.monotonic() < deadline:
        availability = robots.from_database(robot_id)[1].get("availability")
        if availability and code in [reason["code"] for reason in availability["reasons"]]:
            return availability
        time.sleep(0.2)
    raise TimeoutError(f"timeout {timeout:.0f}s waiting for reason {code}\nlast: {availability}")


@pytest.fixture
def graph_map(maps) -> dict:
    code, created = maps.create()
    assert code == 201, created
    return created


# ------------------------------------------------------------------ ошибки запроса (без симулятора)

@pytest.mark.parametrize("body, fields", [
    ({}, {"mapId", "nodeId"}),
    ({"mapId": "m", "nodeId": 5}, {"nodeId"}),
    ({"mapId": "m", "nodeId": "n1", "theta": "north"}, {"theta"}),
])
def test_invalid_body(robots, body, fields):
    code, response = robots.position(NO_ROBOT, body)
    assert code == 400
    assert response["code"] == "INVALID_INPUT"
    assert set(response["details"]) == fields


def test_body_not_json(robots):
    code, response = http("POST", f"{CONTROL}/api/fms/robots/{NO_ROBOT}/position", "not json")
    assert code == 400
    assert response["code"] == "INVALID_INPUT"


def test_unknown_map(robots):
    code, response = robots.position(NO_ROBOT, {"mapId": "test-ctl-map-none", "nodeId": "n1"})
    assert code == 404
    assert response["code"] == "NOT_FOUND"
    assert "test-ctl-map-none" in response["message"]


def test_unknown_node(robots, graph_map):
    code, response = robots.position(NO_ROBOT, {"mapId": graph_map["id"], "nodeId": "n9"})
    assert code == 400
    assert response["code"] == "INVALID_INPUT"
    assert set(response["details"]) == {"nodeId"}


@pytest.mark.parametrize("node_id", ["n1", "st1"])   # станция — тоже узел графа
def test_unknown_robot(robots, graph_map, node_id):
    code, response = robots.position(NO_ROBOT, {"mapId": graph_map["id"], "nodeId": node_id})
    assert code == 404
    assert response["code"] == "NOT_FOUND"


def test_robot_not_online(robots, graph_map):
    """Выдуманный робот: от него ничего не приходило, dispatch команду не шлёт."""
    _, robot = robots.register()
    code, response = robots.position(robot["id"], {"mapId": graph_map["id"], "nodeId": "n1"})
    assert code == 409
    assert response["code"] == "INVALID_STATE"


# ------------------------------------------------------------------ с роботом симулятора

def test_place_on_node(sim_map, robots, sim_robot):
    node_id = sim_map["waypoints"][0]["id"]
    x, y = node_xy(sim_map, node_id)
    status = robots.place(sim_robot, {"mapId": SIM_MAP, "nodeId": node_id, "theta": 0.5}, on_node(node_id, 0.5))

    assert status["pose"]["mapId"] == SIM_MAP
    assert status["pose"]["x"] == pytest.approx(x, abs=1e-3)
    assert status["pose"]["y"] == pytest.approx(y, abs=1e-3)
    assert wait_availability(robots, sim_robot, "IDLE")["reasons"] == []


def test_theta_from_node_or_current_pose(sim_map, robots, sim_robot):
    """Без theta в запросе: у узла есть theta — она, нет — прежняя ориентация робота."""
    node = sim_map["waypoints"][0]
    robots.place(sim_robot, {"mapId": SIM_MAP, "nodeId": node["id"], "theta": 0.5}, on_node(node["id"], 0.5))

    expected = node.get("theta", 0.5)
    robots.place(sim_robot, {"mapId": SIM_MAP, "nodeId": node["id"]}, on_node(node["id"], expected))


def test_availability_follows_maps(maps, robots, sim_robot, dispatch):
    """Карта робота появилась в fms, робот встал на узел, узел убрали из карты — control пересчитывает
    готовность сразу, без нового статуса от робота."""
    if maps.from_database(SIM_MAP)[0] == 200:
        pytest.skip(f"map {SIM_MAP} already exists in fms — not changing someone else's map")
    # Робот на карте SIM_MAP, которой в fms нет: ставим напрямую через dispatch, control тут не пустит
    robots.until_applied(sim_robot, lambda: dispatch.InitializePosition(dpb.InitializePositionRequest(
        robot_id=sim_robot, pose=fms.Pose(x=0, y=0, theta=0, map_id=SIM_MAP), last_node_id="origin")),
        on_node("origin"), "robot on node origin")
    wait_reason(robots, sim_robot, "UNKNOWN_MAP")

    code, created = maps.create(id=SIM_MAP)
    assert code == 201, created
    wait_reason(robots, sim_robot, "NOT_ON_GRAPH")   # origin — не узел этой карты

    robots.place(sim_robot, {"mapId": SIM_MAP, "nodeId": "n1"}, on_node("n1"))
    wait_availability(robots, sim_robot, "IDLE")

    code, updated = maps.patch(SIM_MAP, {"version": created["version"],
                                         "waypoints": [w for w in created["waypoints"] if w["id"] != "n1"],
                                         "edges": []})
    assert code == 200, updated
    wait_reason(robots, sim_robot, "NOT_ON_GRAPH")


def test_position_while_busy(sim_map, robots, sim_robot, dispatch):
    """По VDA робот с заказом initializePosition не примет — control отвечает 409 сам."""
    mission_id = start_mission(robots, dispatch, sim_robot)
    robots.wait_status(sim_robot, lambda s: s["missionId"] == mission_id, 5, "mission in status")

    code, response = robots.position(sim_robot, {"mapId": SIM_MAP, "nodeId": sim_map["waypoints"][0]["id"]})
    assert code == 409
    assert response["code"] == "ROBOT_BUSY"
    assert mission_id in response["message"]
