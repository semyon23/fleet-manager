"""Первое реальное пересечение.

Control должен быть запущен с FMS_TRAFFIC_ENABLED=1.
Тест создаёт собственную map_1; существующую карту не меняет.
Симулятор: amr101 и amr102, initial_state.map_id=map_1.
"""

import os
import time

import pytest

from conftest import CONTROL, SIM_MAP, Missions, http
from test_mission_run import two_robots


@pytest.fixture
def run_map(maps):
    if os.environ.get("RUN_TRAFFIC_LIVE") != "1":
        pytest.skip(
            "requires control with FMS_TRAFFIC_ENABLED=1; "
            "set RUN_TRAFFIC_LIVE=1"
        )

    if maps.from_database(SIM_MAP)[0] == 200:
        pytest.skip(
            "test map already exists; not changing existing map"
        )

    xy = {
        "a": (1, 5),
        "x": (5, 5),
        "b": (9, 5),
        "d": (5, 1),
        "c": (5, 9),
    }

    pairs = [
        ("a", "x"),
        ("x", "b"),
        ("d", "x"),
        ("x", "c"),
    ]

    edges = []
    for a, b in pairs:
        edges.extend([
            {"id": a + b, "from": a, "to": b},
            {"id": b + a, "from": b, "to": a},
        ])

    code, body = maps.create(
        id=SIM_MAP,
        width=220,
        height=220,
        meta={
            "resolution": 0.05,
            "origin": [0, 0, 0],
        },
        waypoints=[
            {
                "id": n,
                "u": round(x / 0.05),
                "v": round(y / 0.05),
            }
            for n, (x, y) in xy.items()
        ],
        edges=edges,
        stations=[],
    )

    assert code == 201, body
    return body


def test_two_robots_cross_without_shared_permission(
        missions,
        robots,
        two_robots,
        run_map,
):
    first, second = two_robots
    ids = []

    for robot, goal in ((first, "b"), (second, "c")):
        code, body = missions.create(
            Missions.body(
                SIM_MAP,
                requestedRobotId=robot,
                steps=[{"nodeId": goal}],
            )
        )

        assert code == 201, body
        ids.append(body["id"])

    deadline = time.monotonic() + 70
    saw_waiting = False
    saw_active = False

    while time.monotonic() < deadline:
        states = []

        for mid in ids:
            code, mission = missions.from_database(mid)
            assert code == 200, mission
            assert mission["state"] not in {
                "FAILED", "CANCELED"
            }, mission
            states.append(mission["state"])

        code, routes = http(
            "GET",
            f"{CONTROL}/api/fms/routes?mapId={SIM_MAP}",
        )
        assert code == 200, routes

        crossing_owners = []

        for route in routes["items"]:
            if route["missionId"] not in ids:
                continue

            if "x" in route["released"]["nodes"]:
                crossing_owners.append(route["robotId"])

            if route["planned"]["nodes"]:
                saw_waiting = True

        assert len(crossing_owners) <= 1, routes

        status = [
            robots.from_database(robot)[1]["status"]
            for robot in (first, second)
        ]

        assert not all(
            s["lastNodeId"] == "x" for s in status
        ), status

        if "RUNNING" in states and not saw_active:
            saw_active = True

            code, body = http(
                "PATCH",
                f"{CONTROL}/api/fms/maps/{SIM_MAP}?force=true",
                {
                    "version": run_map["version"],
                    "name": "must not be saved",
                },
            )

            assert (
                    code == 409 and
                    body["code"] == "TRAFFIC_ACTIVE"
            ), (code, body)

        if states == ["COMPLETED", "COMPLETED"]:
            assert saw_waiting, (
                "traffic was not observed: "
                "check FMS_TRAFFIC_ENABLED"
            )
            assert saw_active
            return

        time.sleep(0.1)

    pytest.fail(
        f"traffic did not complete both missions: {states}"
    )