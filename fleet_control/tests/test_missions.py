"""Миссии (M-3b): POST /missions и POST /missions/:id/cancel в control — контракт UI, раздел 7.

Миссии пока только встают в очередь (QUEUED): выбор робота и исполнение — M-3c.
Проверяем ответ control и то, что увидит UI, — документ в базе (GET /missions базы).
У каждого теста своя карта (фикстура maps): n1 ↔ n2, станция st1 без рёбер с подъездом от n2.
"""

from __future__ import annotations

import re
import threading
import time

import pytest

from conftest import CONTROL, DATABASE_HTTP, Missions, http

ISO_MS = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$")
MISSION_FIELDS = {"id", "name", "mapId", "priority", "requestedRobotId", "robotId", "steps", "state", "currentStep",
                  "stepStates", "error", "createdAt", "assignedAt", "startedAt", "finishedAt", "updatedAt", "createdBy"}


def wait_in_database(missions, mission_id: str, state: str) -> dict:
    """Изменения после создания control пишет в базу через очередь, без ожидания: подождать до 5 с."""
    stored = None
    for _ in range(50):
        _, stored = missions.from_database(mission_id)
        if stored and stored.get("state") == state:
            return stored
        time.sleep(0.1)
    raise TimeoutError(f"mission {mission_id} is not {state} in the database: {stored}")


@pytest.fixture
def map_id(maps) -> str:
    code, created = maps.create()
    assert code == 201, created
    return created["id"]


# ------------------------------------------------------------------ создание

def test_create_mission(missions, map_id):
    code, mission = missions.create(Missions.body(map_id))

    assert code == 201, mission
    assert set(mission) == MISSION_FIELDS
    assert mission["state"] == "QUEUED"
    assert mission["priority"] == "NORMAL"
    assert mission["robotId"] is None and mission["requestedRobotId"] is None
    assert mission["steps"] == [{"nodeId": "n2", "actions": []}]
    assert [s["state"] for s in mission["stepStates"]] == ["PENDING"]
    assert ISO_MS.match(mission["createdAt"])
    assert mission["updatedAt"] == mission["createdAt"]
    assert mission["finishedAt"] is None and mission["createdBy"] is None


def test_database_has_the_same_document(missions, map_id):
    _, mission = missions.create(Missions.body(map_id))
    code, stored = missions.from_database(mission["id"])
    assert code == 200
    assert stored == mission


def test_generated_id(missions, map_id):
    body = Missions.body(map_id)
    del body["id"]
    code, mission = missions.create(body)
    assert code == 201
    assert re.fullmatch(r"m-[a-z0-9]{8}", mission["id"])


def test_full_request_is_kept(missions, map_id, robots):
    _, robot = robots.register()
    steps = [
        {"nodeId": "n1", "actions": [{"actionType": "pick", "blockingType": "HARD",
                                      "actionParameters": [{"key": "loadId", "value": "PALLET-1"}]}]},
        {"nodeId": "n2", "actions": [{"actionType": "drop", "descriptor": "unknown fields are kept"}]},
    ]
    code, mission = missions.create(Missions.body(map_id, priority="HIGH", requestedRobotId=robot["id"], steps=steps))

    assert code == 201, mission
    assert mission["priority"] == "HIGH"
    assert mission["requestedRobotId"] == robot["id"]
    assert mission["robotId"] is None   # робота назначит fms (M-3c)
    assert mission["steps"] == steps
    assert len(mission["stepStates"]) == 2


def test_station_without_edges_is_reached_through_interaction_node(missions, map_id):
    code, mission = missions.create(Missions.body(map_id, steps=[{"nodeId": "n1"}, {"nodeId": "st1"}]))
    assert code == 201, mission


# ------------------------------------------------------------------ ошибки запроса

INVALID = [
    ({"name": None}, "name"),
    ({"name": ""}, "name"),
    ({"mapId": None}, "mapId"),
    ({"id": "bad id!"}, "id"),
    ({"priority": "URGENT"}, "priority"),
    ({"priority": 2}, "priority"),
    ({"requestedRobotId": 5}, "requestedRobotId"),
    ({"steps": None}, "steps"),
    ({"steps": []}, "steps"),
    ({"steps": "n1"}, "steps"),
    ({"steps": ["n1"]}, "steps[0]"),
    ({"steps": [{"actions": []}]}, "steps[0].nodeId"),
    ({"steps": [{"nodeId": "n1"}, {"nodeId": "n2", "actions": {}}]}, "steps[1].actions"),
    ({"steps": [{"nodeId": "n1", "actions": [{}]}]}, "steps[0].actions[0].actionType"),
    ({"steps": [{"nodeId": "n1", "actions": [{"actionType": "pick", "blockingType": "MAYBE"}]}]},
     "steps[0].actions[0].blockingType"),
    ({"steps": [{"nodeId": "n1", "actions": [{"actionType": "pick", "actionParameters": [{"key": "x"}]}]}]},
     "steps[0].actions[0].actionParameters"),
]


@pytest.mark.parametrize("fields, field", INVALID, ids=[f for _, f in INVALID])
def test_invalid_fields(missions, map_id, fields, field):
    body = Missions.body(map_id, **fields)
    code, error = missions.create(body)

    assert code == 400
    assert error["code"] == "INVALID_INPUT"
    assert field in error["details"]
    if isinstance(body.get("id"), str):
        assert missions.from_database(body["id"])[0] == 404   # ничего не сохранено


def test_all_field_errors_at_once(missions, map_id):
    code, error = missions.create(Missions.body(map_id, name="", priority="URGENT",
                                                steps=[{"nodeId": "n1", "actions": [{"blockingType": "MAYBE"}]}]))
    assert code == 400
    assert set(error["details"]) == {"name", "priority", "steps[0].actions[0].actionType",
                                     "steps[0].actions[0].blockingType"}


def test_body_not_json(missions):
    code, error = missions.create("{not json")
    assert code == 400
    assert error["code"] == "INVALID_INPUT"


def test_unknown_map(missions):
    code, error = missions.create(Missions.body("test-ctl-map-none"))
    assert code == 400
    assert set(error["details"]) == {"mapId"}


def test_unknown_node(missions, map_id):
    code, error = missions.create(Missions.body(map_id, steps=[{"nodeId": "n1"}, {"nodeId": "n9"}]))
    assert code == 400
    assert set(error["details"]) == {"steps[1].nodeId"}


def test_unreachable_station(missions, maps):
    """Станция без рёбер и без узлов взаимодействия: доехать до неё нельзя."""
    _, created = maps.create(stations=[{"id": "lost", "u": 90, "v": 30, "kind": "charge"}])
    code, error = missions.create(Missions.body(created["id"], steps=[{"nodeId": "lost"}]))
    assert code == 400
    assert "steps[0].nodeId" in error["details"]


def test_no_route_between_steps(missions, maps):
    """n1 ↔ n2 → trap: из тупика не выехать, шаг trap → n1 невозможен."""
    _, created = maps.create(force=True,
                             waypoints=[{"id": "n1", "u": 10, "v": 20}, {"id": "n2", "u": 50, "v": 20},
                                        {"id": "trap", "u": 90, "v": 20}],
                             edges=[{"id": "a", "from": "n1", "to": "n2"}, {"id": "b", "from": "n2", "to": "n1"},
                                    {"id": "c", "from": "n2", "to": "trap"}],
                             stations=[])
    code, error = missions.create(Missions.body(created["id"], steps=[{"nodeId": "n1"}, {"nodeId": "trap"}]))
    assert code == 201, error   # туда можно

    code, error = missions.create(Missions.body(created["id"], steps=[{"nodeId": "trap"}, {"nodeId": "n1"}]))
    assert code == 400
    assert error["code"] == "INVALID_MISSION"
    assert "trap" in error["message"] and "n1" in error["message"]


def test_unknown_requested_robot(missions, map_id):
    code, error = missions.create(Missions.body(map_id, requestedRobotId="test-ctl-no-such-robot"))
    assert code == 400
    assert set(error["details"]) == {"requestedRobotId"}


# ------------------------------------------------------------------ повтор с тем же id

def test_repeat_returns_the_same_mission(missions, map_id):
    body = Missions.body(map_id)
    _, first = missions.create(body)
    code, again = missions.create(body)

    assert code == 200
    assert again == first   # та же миссия, второй не появилось


def test_repeat_with_defaults_spelled_out_is_the_same(missions, map_id):
    """Без приоритета и с NORMAL, без actions и с [] — один и тот же запрос."""
    body = Missions.body(map_id)
    missions.create(body)
    code, _ = missions.create({**body, "priority": "NORMAL", "steps": [{"nodeId": "n2", "actions": []}]})
    assert code == 200


def test_same_id_different_request(missions, map_id):
    body = Missions.body(map_id)
    _, first = missions.create(body)
    code, error = missions.create({**body, "steps": [{"nodeId": "n1"}]})

    assert code == 409
    assert error["code"] == "ALREADY_EXISTS"
    assert missions.from_database(body["id"])[1] == first   # первая не тронута


def test_concurrent_same_requests_create_one_mission(missions, map_id):
    """Пять одинаковых POST одновременно (WMS повторяет после обрыва): одна 201, остальные 200, миссия одна."""
    body = Missions.body(map_id)
    results: list[tuple[int, dict]] = []

    def post() -> None:
        results.append(http("POST", f"{CONTROL}/api/fms/missions", body))

    threads = [threading.Thread(target=post) for _ in range(5)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    missions.ids.append(body["id"])
    Missions.CREATED.append(body["id"])

    assert sorted(code for code, _ in results) == [200, 200, 200, 200, 201]
    assert len({mission["createdAt"] for _, mission in results}) == 1


def test_repeat_of_finished_mission(missions, map_id):
    """Миссия уже завершена (её нет в памяти control) — повтор находит её в базе."""
    body = Missions.body(map_id)
    missions.create(body)
    _, cancelled = missions.cancel(body["id"])
    code, again = missions.create(body)
    assert code == 200
    assert again["state"] == "CANCELED"
    assert again == cancelled


# ------------------------------------------------------------------ отмена

def test_cancel_queued_mission(missions, map_id):
    _, mission = missions.create(Missions.body(map_id, steps=[{"nodeId": "n1"}, {"nodeId": "n2"}]))
    code, cancelled = missions.cancel(mission["id"])

    assert code == 202
    assert cancelled["state"] == "CANCELED"
    assert [s["state"] for s in cancelled["stepStates"]] == ["CANCELED", "CANCELED"]
    assert ISO_MS.match(cancelled["finishedAt"])
    assert cancelled["updatedAt"] == cancelled["finishedAt"]
    assert cancelled["createdAt"] == mission["createdAt"]


def test_cancelled_mission_in_database(missions, map_id):
    _, mission = missions.create(Missions.body(map_id))
    _, cancelled = missions.cancel(mission["id"])
    assert wait_in_database(missions, mission["id"], "CANCELED") == cancelled


def test_cancel_twice(missions, map_id):
    _, mission = missions.create(Missions.body(map_id))
    missions.cancel(mission["id"])
    code, error = missions.cancel(mission["id"])
    assert code == 409
    assert error["code"] == "INVALID_STATE"
    assert "CANCELED" in error["message"]


def test_cancel_unknown_mission(missions):
    code, error = missions.cancel("test-ctl-m-nope")
    assert code == 404
    assert error["code"] == "NOT_FOUND"


# ------------------------------------------------------------------ история для UI

def test_history_by_map(missions, map_id):
    """GET /missions базы: отменённая остаётся в истории, фильтр по состоянию её отделяет."""
    _, first = missions.create(Missions.body(map_id))
    _, second = missions.create(Missions.body(map_id))
    missions.cancel(first["id"])
    wait_in_database(missions, first["id"], "CANCELED")

    code, page = http("GET", f"{DATABASE_HTTP}/api/fms/missions?mapId={map_id}")
    assert code == 200
    assert {m["id"] for m in page["items"]} == {first["id"], second["id"]}

    _, page = http("GET", f"{DATABASE_HTTP}/api/fms/missions?mapId={map_id}&state=QUEUED")
    assert [m["id"] for m in page["items"]] == [second["id"]]
    _, page = http("GET", f"{DATABASE_HTTP}/api/fms/missions?mapId={map_id}&state=CANCELED")
    assert [m["id"] for m in page["items"]] == [first["id"]]
