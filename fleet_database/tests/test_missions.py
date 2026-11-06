"""Миссии (M-3): ControlStorage — то, что пишет control, и REST для UI (docs/API_CONTRACT.md, раздел 7.2).

Миссии тесты пишут через gRPC вместо control, читают — через gRPC и REST.
Чтобы чужие миссии в таблице не мешали спискам REST, у каждого теста своя карта (mapId) — список
фильтруется по ней.
"""

from __future__ import annotations

import json
import time

import grpc
import pytest

import fleet_database_pb2 as db
from conftest import error_code, unique
from test_rest import get

ACTIVE = ["QUEUED", "ASSIGNED", "RUNNING", "PAUSED", "CANCELING"]
FINISHED = ["COMPLETED", "FAILED", "CANCELED"]


def body(record: db.MissionRecord) -> dict:
    """body_json как объект: JSONB в базе меняет порядок ключей и пробелы, но не содержимое."""
    return json.loads(record.body_json)


def now_ms() -> int:
    return int(time.time() * 1000)


def listing(query: str) -> tuple[list[str], str | None]:
    """GET /missions?<query> → (id миссий страницы по порядку, nextCursor)."""
    status, page = get(f"/api/fms/missions?{query}")
    assert status == 200, page
    assert set(page) == {"items", "nextCursor"}
    return [item["id"] for item in page["items"]], page["nextCursor"]


def all_pages(query: str) -> list[list[str]]:
    """Пройти все страницы по nextCursor → id по страницам."""
    pages, cursor = [], None
    while True:
        ids, cursor = listing(query + (f"&before={cursor}" if cursor else ""))
        pages.append(ids)
        if cursor is None:
            return pages
        assert len(pages) < 100, "cursor does not advance"


# ------------------------------------------------------------------ SaveMission / GetMission

def test_save_and_get(missions):
    saved = missions.save()
    got = missions.get(saved.mission_id)

    assert got.map_id == saved.map_id
    assert got.robot_id == ""                    # не назначен: в базе NULL
    assert got.state == "QUEUED"
    assert got.priority == 1
    assert body(got) == body(saved)
    assert got.created_at == saved.created_at    # времена ставит control, база хранит как есть
    assert got.updated_at == saved.updated_at
    assert not got.HasField("finished_at")


def test_save_replaces_whole_record(missions):
    first = missions.save(created_ms=1_000)
    second = missions.record(mission_id=first.mission_id, created_ms=2_000, map_id="test-map-other",
                             robot_id="amr-01", state="COMPLETED", priority=3,
                             body_json=json.dumps({"id": first.mission_id, "state": "COMPLETED"}))
    second.finished_at.FromMilliseconds(3_000)
    missions.control.SaveMission(db.SaveMissionRequest(mission=second))

    got = missions.get(first.mission_id)
    assert (got.map_id, got.robot_id, got.state, got.priority) == ("test-map-other", "amr-01", "COMPLETED", 3)
    assert body(got) == {"id": first.mission_id, "state": "COMPLETED"}
    assert got.created_at.ToMilliseconds() == 2_000
    assert got.finished_at.ToMilliseconds() == 3_000


def test_robot_can_be_unassigned_again(missions):
    """Назначение снято (робот удалён до старта) — снова пусто, а не прежний робот."""
    saved = missions.save(robot_id="amr-01", state="ASSIGNED")
    missions.control.SaveMission(db.SaveMissionRequest(mission=missions.record(
        mission_id=saved.mission_id, robot_id="", state="QUEUED")))
    assert missions.get(saved.mission_id).robot_id == ""


@pytest.mark.parametrize("fields", [{"state": "DONE"}, {"state": ""}, {"priority": 4}])
def test_invalid_state_or_priority(missions, fields):
    record = missions.record(**fields)
    assert error_code(missions.control.SaveMission, db.SaveMissionRequest(mission=record)) == \
        grpc.StatusCode.INVALID_ARGUMENT
    assert error_code(missions.control.GetMission, db.GetMissionRequest(mission_id=record.mission_id)) == \
        grpc.StatusCode.NOT_FOUND   # ничего не записалось


def test_body_not_json(missions):
    record = missions.record(body_json="{not json")
    assert error_code(missions.control.SaveMission, db.SaveMissionRequest(mission=record)) == \
        grpc.StatusCode.INVALID_ARGUMENT


@pytest.mark.parametrize("missing", ["mission_id", "map_id", "body_json", "created_at", "updated_at"])
def test_required_fields(missions, missing):
    record = missions.record()
    record.ClearField(missing)
    assert error_code(missions.control.SaveMission, db.SaveMissionRequest(mission=record)) == \
        grpc.StatusCode.INVALID_ARGUMENT


def test_get_unknown(missions):
    request = db.GetMissionRequest(mission_id=unique("test-mission"))
    assert error_code(missions.control.GetMission, request) == grpc.StatusCode.NOT_FOUND


# ------------------------------------------------------------------ ListActiveMissions

def test_active_missions_only_unfinished_by_created_at(missions):
    base = now_ms()
    # Сохраняем в обратном порядке времени: ответ должен быть по created_at, а не по порядку записи
    saved = {state: missions.save(created_ms=base - i * 1000, state=state)
             for i, state in enumerate(ACTIVE + FINISHED)}

    active = missions.active()
    assert [m.state for m in active] == list(reversed(ACTIVE))
    assert {m.mission_id for m in active} == {saved[state].mission_id for state in ACTIVE}


def test_finished_mission_leaves_active_list(missions):
    saved = missions.save(state="RUNNING", robot_id="amr-01")
    assert [m.mission_id for m in missions.active()] == [saved.mission_id]

    done = missions.record(mission_id=saved.mission_id, state="COMPLETED", robot_id="amr-01")
    done.finished_at.FromMilliseconds(now_ms())
    missions.control.SaveMission(db.SaveMissionRequest(mission=done))
    assert missions.active() == []


# ------------------------------------------------------------------ REST: одна миссия

def test_rest_mission_is_body_as_is(missions):
    document = {"id": "x", "name": "Pallet A3 → Dock 2", "steps": [{"nodeId": "st-a3", "actions": []}],
                "requestedRobotId": None, "stepStates": [], "error": None}
    saved = missions.save(body_json=json.dumps(document))
    status, mission = get(f"/api/fms/missions/{saved.mission_id}")
    assert status == 200
    assert mission == document   # база отдаёт документ control, ничего не добавляя


def test_rest_unknown_mission():
    status, error = get(f"/api/fms/missions/{unique('test-mission')}")
    assert status == 404
    assert error["code"] == "NOT_FOUND"
    assert set(error) == {"code", "message", "details"}


# ------------------------------------------------------------------ REST: список и фильтры

def test_rest_list_newest_first(missions):
    map_id = unique("test-map")
    base = now_ms()
    saved = [missions.save(created_ms=base + i, map_id=map_id) for i in range(3)]

    ids, cursor = listing(f"mapId={map_id}")
    assert ids == [m.mission_id for m in reversed(saved)]
    assert cursor is None


def test_rest_items_are_documents(missions):
    map_id = unique("test-map")
    saved = missions.save(map_id=map_id)
    status, page = get(f"/api/fms/missions?mapId={map_id}")
    assert status == 200
    assert page["items"] == [body(saved)]


@pytest.mark.parametrize("separator", [",", "%2C"])   # UI шлёт запятую закодированной
def test_rest_filter_by_states(missions, separator):
    map_id = unique("test-map")
    base = now_ms()
    by_state = {state: missions.save(created_ms=base + i, map_id=map_id, state=state)
                for i, state in enumerate(ACTIVE + FINISHED)}

    ids, _ = listing(f"mapId={map_id}&state=QUEUED{separator}RUNNING{separator}FAILED")
    assert ids == [by_state[s].mission_id for s in ("FAILED", "RUNNING", "QUEUED")]


def test_rest_filter_by_robot(missions):
    map_id = unique("test-map")
    robot_id = unique("test-robot")
    mine = missions.save(map_id=map_id, robot_id=robot_id, state="RUNNING")
    missions.save(map_id=map_id, robot_id=unique("test-robot"), state="RUNNING")
    missions.save(map_id=map_id)   # не назначена

    ids, _ = listing(f"robotId={robot_id}")
    assert ids == [mine.mission_id]


def test_rest_filters_combine(missions):
    map_id = unique("test-map")
    robot_id = unique("test-robot")
    running = missions.save(map_id=map_id, robot_id=robot_id, state="RUNNING")
    missions.save(map_id=map_id, robot_id=robot_id, state="COMPLETED")
    missions.save(map_id=unique("test-map"), robot_id=robot_id, state="RUNNING")

    ids, _ = listing(f"mapId={map_id}&robotId={robot_id}&state=RUNNING")
    assert ids == [running.mission_id]


# ------------------------------------------------------------------ REST: страницы

def test_rest_pages_cover_everything_once(missions):
    map_id = unique("test-map")
    base = now_ms()
    saved = [missions.save(created_ms=base + i, map_id=map_id) for i in range(7)]

    pages = all_pages(f"mapId={map_id}&limit=3")
    assert [len(page) for page in pages] == [3, 3, 1]
    assert sum(pages, []) == [m.mission_id for m in reversed(saved)]


def test_rest_pages_with_equal_created_at(missions):
    """Одинаковое время создания — порядок по mission_id, курсор не теряет и не повторяет миссии."""
    map_id = unique("test-map")
    ms = now_ms()
    saved = [missions.save(created_ms=ms, map_id=map_id) for _ in range(5)]

    pages = all_pages(f"mapId={map_id}&limit=2")
    assert [len(page) for page in pages] == [2, 2, 1]
    assert sum(pages, []) == sorted((m.mission_id for m in saved), reverse=True)


def test_rest_last_full_page_has_no_cursor(missions):
    """Миссий ровно limit — следующей страницы нет, а не пустая следующая."""
    map_id = unique("test-map")
    for _ in range(3):
        missions.save(map_id=map_id)
    ids, cursor = listing(f"mapId={map_id}&limit=3")
    assert len(ids) == 3
    assert cursor is None


def test_rest_default_and_max_limit(missions):
    map_id = unique("test-map")
    base = now_ms()
    for i in range(51):
        missions.save(created_ms=base + i, map_id=map_id)

    ids, cursor = listing(f"mapId={map_id}")
    assert len(ids) == 50 and cursor is not None    # по умолчанию 50
    ids, cursor = listing(f"mapId={map_id}&limit=200")
    assert len(ids) == 51 and cursor is None


@pytest.mark.parametrize("query, field", [
    ("state=DONE", "state"),
    ("state=QUEUED,done", "state"),
    ("limit=0", "limit"),
    ("limit=201", "limit"),
    ("limit=abc", "limit"),
    ("limit=10x", "limit"),
    ("before=abc", "before"),
    ("before=x:m-1", "before"),
])
def test_rest_invalid_query(query, field):
    status, error = get(f"/api/fms/missions?{query}")
    assert status == 400
    assert error["code"] == "INVALID_INPUT"
    assert set(error["details"]) == {field}


def test_rest_all_query_errors_at_once():
    status, error = get("/api/fms/missions?state=DONE&limit=0&before=abc")
    assert status == 400
    assert set(error["details"]) == {"state", "limit", "before"}
