"""Миссии (M-3): ControlStorage — то, что пишет control, и REST для UI (docs/API_CONTRACT.md, раздел 7.2).
События миссий (c3): пишутся вместе с миссией в SaveMission, читаются через GET /missions/{id}/events.

Миссии тесты пишут через gRPC вместо control, читают — через gRPC и REST.
Чтобы чужие миссии в таблице не мешали спискам REST, у каждого теста своя карта (mapId) — список
фильтруется по ней.
"""

from __future__ import annotations

import json
import subprocess
import time
from datetime import datetime, timezone

import grpc
import pytest

import fleet_database_pb2 as db
from conftest import DATABASE_URL, Missions, error_code, unique
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
    ("from=yesterday", "from"),
    ("to=2026-13-01T00:00:00Z", "to"),
    ("from=2026-11-02T00:00:00Z&to=2026-11-01T00:00:00Z", "to"),   # to раньше from
    ("from=2026-11-01T00:00:00Z&to=2026-11-01T00:00:00Z", "to"),   # пустой полуинтервал
])
def test_rest_invalid_query(query, field):
    status, error = get(f"/api/fms/missions?{query}")
    assert status == 400
    assert error["code"] == "INVALID_INPUT"
    assert set(error["details"]) == {field}


def test_rest_all_query_errors_at_once():
    status, error = get("/api/fms/missions?state=DONE&limit=0&before=abc&from=x&to=y")
    assert status == 400
    assert set(error["details"]) == {"state", "limit", "before", "from", "to"}


# ------------------------------------------------------------------ REST: период ?from=&to= (c3)

T = 1_790_000_000_000   # 2026-09-21T14:13:20.000Z


def iso(ms: int) -> str:
    """Время контракта: ISO UTC с миллисекундами."""
    return datetime.fromtimestamp(ms // 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S") + f".{ms % 1000:03d}Z"


def test_rest_filter_by_period(missions):
    """[from, to): from входит, to — нет; новые сверху, как без фильтра."""
    map_id = unique("test-map")
    at = {offset: missions.save(created_ms=T + offset, map_id=map_id) for offset in (-1000, 0, 1000, 2000)}

    ids, _ = listing(f"mapId={map_id}&from={iso(T)}&to={iso(T + 2000)}")
    assert ids == [at[1000].mission_id, at[0].mission_id]
    ids, _ = listing(f"mapId={map_id}&from={iso(T + 1000)}")
    assert ids == [at[2000].mission_id, at[1000].mission_id]
    ids, _ = listing(f"mapId={map_id}&to={iso(T)}")
    assert ids == [at[-1000].mission_id]


def test_rest_period_with_offset_and_fraction(missions):
    """Время с долями секунды и смещением: 17:13:20.500+03:00 — это T + 500 мс. «+» в запросе — %2B."""
    map_id = unique("test-map")
    missions.save(created_ms=T + 499, map_id=map_id)
    later = missions.save(created_ms=T + 500, map_id=map_id)

    ids, _ = listing(f"mapId={map_id}&from=2026-09-21T17:13:20.500%2B03:00")
    assert ids == [later.mission_id]


def test_rest_pages_inside_period(missions):
    """Курсор и период вместе: страницы проходят только миссии периода."""
    map_id = unique("test-map")
    inside = [missions.save(created_ms=T + i * 100, map_id=map_id) for i in range(5)]
    missions.save(created_ms=T - 100, map_id=map_id)
    missions.save(created_ms=T + 500, map_id=map_id)

    pages = all_pages(f"mapId={map_id}&from={iso(T)}&to={iso(T + 500)}&limit=2")
    assert [len(page) for page in pages] == [2, 2, 1]
    assert sum(pages, []) == [m.mission_id for m in reversed(inside)]


# ------------------------------------------------------------------ события миссии (c3)

def events_of(mission_id: str) -> list[dict]:
    status, page = get(f"/api/fms/missions/{mission_id}/events")
    assert status == 200, page
    assert set(page) == {"items"}
    return page["items"]


def test_events_saved_with_mission(missions):
    """Событие в REST — поля строки (id, missionId, at, robotId, type) + details как есть."""
    saved = missions.save(events=[
        Missions.event("STATE", {"state": "QUEUED"}, at_ms=T + 123),
        Missions.event("STATE", {"state": "ASSIGNED", "next": True}, at_ms=T + 456, robot_id="amr-01"),
    ])

    first, second = events_of(saved.mission_id)
    assert isinstance(first["id"], int) and first["id"] < second["id"]
    assert first == {"id": first["id"], "missionId": saved.mission_id, "at": "2026-09-21T14:13:20.123Z",
                     "robotId": None, "type": "STATE", "state": "QUEUED"}
    assert second == {"id": second["id"], "missionId": saved.mission_id, "at": "2026-09-21T14:13:20.456Z",
                      "robotId": "amr-01", "type": "STATE", "state": "ASSIGNED", "next": True}


def test_events_accumulate_in_order(missions):
    """Каждая запись миссии дописывает свои события; порядок — порядок записи (id), а не время at."""
    saved = missions.save(events=[Missions.event("STATE", {"state": "QUEUED"}, at_ms=T + 5000)])
    missions.save(mission_id=saved.mission_id, state="RUNNING", robot_id="amr-01", events=[
        Missions.event("STATE", {"state": "RUNNING"}, at_ms=T, robot_id="amr-01"),
        Missions.event("STEP", {"stepIndex": 0, "state": "RUNNING"}, at_ms=T, robot_id="amr-01"),
    ])
    missions.save(mission_id=saved.mission_id, state="RUNNING", robot_id="amr-01")   # без событий

    events = events_of(saved.mission_id)
    assert [(e["type"], e["state"]) for e in events] == [("STATE", "QUEUED"), ("STATE", "RUNNING"),
                                                         ("STEP", "RUNNING")]
    assert [e["id"] for e in events] == sorted(e["id"] for e in events)


def test_event_kinds_keep_details(missions):
    """STEP и ACTION, вложенные объекты (error) — как прислал control."""
    error = {"code": "ACTION_FAILED", "message": "gripper jammed"}
    saved = missions.save(events=[
        Missions.event("STEP", {"stepIndex": 1, "state": "COMPLETED"}),
        Missions.event("ACTION", {"stepIndex": 1, "actionId": "a-17", "actionType": "pick", "status": "FAILED"}),
        Missions.event("STATE", {"state": "FAILED", "error": error}),
    ])

    step, action, failed = events_of(saved.mission_id)
    assert (step["type"], step["stepIndex"], step["state"]) == ("STEP", 1, "COMPLETED")
    assert (action["type"], action["actionId"], action["actionType"], action["status"]) == \
        ("ACTION", "a-17", "pick", "FAILED")
    assert failed["error"] == error


def test_row_fields_win_over_details(missions):
    """В details не подменить id, missionId, at, robotId и type события."""
    saved = missions.save(events=[Missions.event(
        "STATE", {"state": "QUEUED", "id": "fake", "missionId": "other", "at": "never", "robotId": "x", "type": "Y"},
        at_ms=T)])
    event, = events_of(saved.mission_id)
    assert isinstance(event["id"], int)
    assert (event["missionId"], event["at"], event["robotId"], event["type"]) == \
        (saved.mission_id, iso(T), None, "STATE")


def test_event_without_details(missions):
    saved = missions.save(events=[Missions.event("STATE", "", at_ms=T)])
    event, = events_of(saved.mission_id)
    assert set(event) == {"id", "missionId", "at", "robotId", "type"}


def test_mission_without_events(missions):
    assert events_of(missions.save().mission_id) == []


def test_events_of_unknown_mission():
    status, error = get(f"/api/fms/missions/{unique('test-mission')}/events")
    assert status == 404
    assert error["code"] == "NOT_FOUND"


def no_at() -> db.MissionEvent:
    event = Missions.event("STATE", {"state": "QUEUED"})
    event.ClearField("at")
    return event


@pytest.mark.parametrize("event", [
    Missions.event("DONE", {"state": "QUEUED"}),   # неизвестный тип
    Missions.event("", {}),
    no_at(),
    Missions.event("STATE", "[1, 2]"),             # не объект — склеился бы с полями строки в массив
    Missions.event("STATE", "\"text\""),
    Missions.event("STATE", "{not json"),
], ids=["unknown type", "empty type", "no at", "array", "string", "not json"])
def test_invalid_event_saves_nothing(missions, event):
    """Ошибка в событии — INVALID_ARGUMENT, и не записаны ни события, ни сама миссия."""
    good = Missions.event("STATE", {"state": "QUEUED"})
    record = missions.record()
    request = db.SaveMissionRequest(mission=record, events=[good, event])
    assert error_code(missions.control.SaveMission, request) == grpc.StatusCode.INVALID_ARGUMENT
    assert error_code(missions.control.GetMission, db.GetMissionRequest(mission_id=record.mission_id)) == \
        grpc.StatusCode.NOT_FOUND


def test_failed_save_writes_no_events(missions):
    """Миссия не записалась (CHECK на state) — её события тоже: одна транзакция."""
    saved = missions.save(events=[Missions.event("STATE", {"state": "QUEUED"})])
    bad = missions.record(mission_id=saved.mission_id, state="DONE")
    request = db.SaveMissionRequest(mission=bad, events=[Missions.event("STATE", {"state": "DONE"})])
    assert error_code(missions.control.SaveMission, request) == grpc.StatusCode.INVALID_ARGUMENT

    assert [e["state"] for e in events_of(saved.mission_id)] == ["QUEUED"]
    assert missions.get(saved.mission_id).state == "QUEUED"


def psql(sql: str) -> str:
    try:
        result = subprocess.run(["psql", DATABASE_URL, "-tAc", sql], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as e:
        pytest.skip(f"psql is not available: {e}")
    if result.returncode != 0:
        pytest.skip(f"psql failed: {result.stderr.strip()}")
    return result.stdout.strip()


def test_events_deleted_with_mission(missions):
    """Удаление миссии (очистка тестов, позже — по сроку хранения) уносит её события: ON DELETE CASCADE."""
    saved = missions.save(events=[Missions.event("STATE", {"state": "QUEUED"}),
                                  Missions.event("STATE", {"state": "CANCELED"})])
    count = f"SELECT count(*) FROM mission_events WHERE mission_id = '{saved.mission_id}'"
    assert psql(count) == "2"

    psql(f"DELETE FROM missions WHERE mission_id = '{saved.mission_id}'")
    assert psql(count) == "0"
