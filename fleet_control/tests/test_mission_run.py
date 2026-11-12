"""Миссии едут (M-3c): назначение роботу, исполнение через dispatch, статусы, отмена — с двумя роботами симулятора.

Нужен симулятор с роботами ktc/amr1 и ktc/amr2 (оба на карте map_1). Без него тесты пропускаются.
Карта map_1 — своя, на время теста; если map_1 уже есть в fms, тесты пропускаются: чужую карту не меняем.

    a ──4 м── b ──4 м── c ──────8 м────── d        дороги в обе стороны, 1 м/с
    amr1                                    amr2   так роботы стоят в начале каждого теста
"""

from __future__ import annotations

import time

import pytest

from conftest import SIM_MANUFACTURER, SIM_MAP, Missions
from test_availability import wait_availability

RESOLUTION = 0.05
NODES = {"a": 1, "b": 5, "c": 9, "d": 17}   # x, м; y у всех 2,5 м


def action(seconds: float) -> dict:
    """Действие симулятора: стоит на месте seconds секунд."""
    return {"actionType": "simulatedAction", "actionParameters": [{"key": "duration", "value": seconds}]}


def wait_mission(missions, mission_id: str, states: set[str], timeout: float = 40) -> dict:
    """Ждать, пока миссия в базе (то, что видит UI) окажется в одном из states."""
    deadline = time.monotonic() + timeout
    mission = None
    while time.monotonic() < deadline:
        code, mission = missions.from_database(mission_id)
        if code == 200 and mission["state"] in states:
            return mission
        time.sleep(0.2)
    raise TimeoutError(f"mission {mission_id} is not {states} after {timeout:.0f} s: {mission}")


def wait_robot(robots, robot_id: str, check, what: str, timeout: float = 10) -> dict:
    deadline = time.monotonic() + timeout
    robot = None
    while time.monotonic() < deadline:
        _, robot = robots.from_database(robot_id)
        if check(robot):
            return robot
        time.sleep(0.2)
    raise TimeoutError(f"robot {robot_id}: no {what} after {timeout:.0f} s: {robot}")


@pytest.fixture
def run_map(maps) -> dict:
    if maps.from_database(SIM_MAP)[0] == 200:
        pytest.skip(f"map {SIM_MAP} already exists in fms — not changing someone else's map")
    waypoints = [{"id": n, "u": round(x / RESOLUTION), "v": 50} for n, x in NODES.items()]
    pairs = [("a", "b"), ("b", "c"), ("c", "d")]
    edges = [{"id": f"{p}{q}", "from": p, "to": q} for p, q in pairs] + \
            [{"id": f"{q}{p}", "from": q, "to": p} for p, q in pairs]
    code, created = maps.create(id=SIM_MAP, width=400, height=100,
                                meta={"resolution": RESOLUTION, "origin": [0, 0, 0]},
                                waypoints=waypoints, edges=edges, stations=[])
    assert code == 201, created
    return created


@pytest.fixture
def two_robots(robots, run_map) -> tuple[str, str]:
    """amr1 на a, amr2 на d, оба IDLE."""
    ids = []
    for serial, node in (("amr1", "a"), ("amr2", "d")):
        code, robot = robots.register(manufacturer=SIM_MANUFACTURER, serialNumber=serial)
        if code == 409:
            pytest.fail(f"{SIM_MANUFACTURER}/{serial} is already registered under another id — delete it first")
        assert code == 201, robot
        try:
            robots.wait_status(robot["id"], lambda s: s["connection"] == "ONLINE", 10, "ONLINE")
        except TimeoutError:
            pytest.skip(f"simulator robot {SIM_MANUFACTURER}/{serial} is not ONLINE — is the simulator running?")
        robots.place(robot["id"], {"mapId": SIM_MAP, "nodeId": node}, lambda s, n=node: s["lastNodeId"] == n)
        ids.append(robot["id"])
    for robot_id in ids:
        wait_availability(robots, robot_id, "IDLE", timeout=10)
    return ids[0], ids[1]


# ------------------------------------------------------------------ миссия доезжает

def test_mission_completes(missions, robots, two_robots):
    """Доехать до b и постоять там 1 с: назначена, едет, выполнена; робот снова свободен на b."""
    amr1, _ = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b", "actions": [action(1)]}]))

    mission = wait_mission(missions, created["id"], {"COMPLETED", "FAILED", "CANCELED"})

    assert mission["state"] == "COMPLETED", mission
    assert mission["robotId"] == amr1                     # ближе к b
    assert mission["error"] is None
    assert mission["assignedAt"] and mission["startedAt"] and mission["finishedAt"]
    step = mission["stepStates"][0]
    assert step["state"] == "COMPLETED"
    assert step["startedAt"] and step["finishedAt"]
    assert [(a["actionType"], a["status"]) for a in step["actions"]] == [("simulatedAction", "FINISHED")]
    assert step["actions"][0]["actionId"]                 # его выдал dispatch

    robot = wait_robot(robots, amr1, lambda r: r["availability"]["state"] == "IDLE", "IDLE")
    assert robot["status"]["lastNodeId"] == "b"
    assert not robot["status"]["missionId"]


def test_nearest_robot_gets_the_mission(missions, two_robots):
    """К d ближе amr2 (он там стоит), к a — amr1."""
    amr1, amr2 = two_robots
    _, to_d = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "d"}]))
    assert wait_mission(missions, to_d["id"], {"COMPLETED"})["robotId"] == amr2

    _, to_a = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "a"}]))
    assert wait_mission(missions, to_a["id"], {"COMPLETED"})["robotId"] == amr1


def test_two_steps(missions, two_robots):
    """b с действием, потом обратно на a с действием — шаги выполняются по порядку."""
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b", "actions": [action(1)]},
                                                                {"nodeId": "a", "actions": [action(1)]}]))
    mission = wait_mission(missions, created["id"], {"COMPLETED", "FAILED", "CANCELED"})

    assert mission["state"] == "COMPLETED", mission
    first, second = mission["stepStates"]
    assert first["state"] == second["state"] == "COMPLETED"
    assert first["finishedAt"] <= second["startedAt"]


def test_requested_robot(missions, two_robots):
    """К b ближе amr1 (4 м против 12), но попросили amr2 — едет amr2."""
    _, amr2 = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr2, steps=[{"nodeId": "b"}]))
    mission = wait_mission(missions, created["id"], {"COMPLETED", "FAILED"})
    assert mission["robotId"] == amr2


# ------------------------------------------------------------------ очередь

def test_mission_waits_for_busy_robot(missions, two_robots):
    """Обе миссии — для amr1. Вторая ждёт в очереди и уходит ему, когда он закончит первую."""
    amr1, _ = two_robots
    _, first = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1,
                                             steps=[{"nodeId": "a", "actions": [action(3)]}]))
    wait_mission(missions, first["id"], {"RUNNING"})
    _, second = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, steps=[{"nodeId": "b"}]))

    assert wait_mission(missions, second["id"], {"QUEUED"})["robotId"] is None   # пока amr1 занят
    done = wait_mission(missions, first["id"], {"COMPLETED"})
    second = wait_mission(missions, second["id"], {"COMPLETED"})
    assert second["robotId"] == amr1
    assert second["assignedAt"] >= done["finishedAt"]


def test_higher_priority_goes_first(missions, two_robots):
    """amr1 занят; в очередь к нему встают LOW, а за ней HIGH. Освободившись, он берёт сначала HIGH."""
    amr1, _ = two_robots
    _, busy = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1,
                                            steps=[{"nodeId": "a", "actions": [action(3)]}]))
    wait_mission(missions, busy["id"], {"RUNNING"})
    _, low = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, priority="LOW", steps=[{"nodeId": "b"}]))
    _, high = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, priority="HIGH", steps=[{"nodeId": "a"}]))

    high = wait_mission(missions, high["id"], {"COMPLETED"}, timeout=60)
    low = wait_mission(missions, low["id"], {"COMPLETED"}, timeout=60)
    assert high["assignedAt"] < low["assignedAt"]


# ------------------------------------------------------------------ отмена

def test_cancel_running_mission(missions, robots, two_robots):
    """Действие на 30 с; отмена — сразу CANCELING, когда робот подтвердит — CANCELED; робот свободен."""
    amr1, _ = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1,
                                               steps=[{"nodeId": "a", "actions": [action(30)]}]))
    wait_mission(missions, created["id"], {"RUNNING"})

    code, cancelling = missions.cancel(created["id"])
    assert code == 202, cancelling
    assert cancelling["state"] in ("CANCELING", "CANCELED")

    mission = wait_mission(missions, created["id"], {"CANCELED", "FAILED", "COMPLETED"}, timeout=15)
    assert mission["state"] == "CANCELED", mission
    wait_robot(robots, amr1, lambda r: r["availability"]["state"] == "IDLE", "IDLE")

    code, error = missions.cancel(created["id"])
    assert code == 409


def test_cancelled_between_nodes_is_not_on_graph(missions, robots, two_robots):
    """Отменили на полпути a → d: симулятор встаёт сразу, посреди ребра. fms не считает его стоящим на узле
    и не даёт ему миссий, пока его не поставят на узел."""
    amr1, _ = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, steps=[{"nodeId": "d"}]))
    wait_mission(missions, created["id"], {"RUNNING"})
    wait_robot(robots, amr1, lambda r: r["status"]["driving"], "driving")
    time.sleep(2.5)                                      # отъехал от a, до b ещё не доехал

    missions.cancel(created["id"])
    wait_mission(missions, created["id"], {"CANCELED"}, timeout=15)

    robot = wait_robot(robots, amr1, lambda r: r["availability"]["state"] == "NOT_READY", "NOT_READY")
    messages = " ".join(reason["message"] for reason in robot["availability"]["reasons"])
    assert "between nodes" in messages, robot["availability"]

    # Поставили на узел — снова свободен
    robots.place(amr1, {"mapId": SIM_MAP, "nodeId": "b"}, lambda s: s["lastNodeId"] == "b")
    wait_robot(robots, amr1, lambda r: r["availability"]["state"] == "IDLE", "IDLE")
