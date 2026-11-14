"""Журнал и тревоги (M-5): ControlStorage — AppendEvents, SaveAlarm, ListOpenAlarms (пишет control) и REST для UI:
GET /events, /alarms, /alarms/{id}. События миссий — в той же таблице, но только через SaveMission.

Чтобы чужие записи не мешали спискам, у каждого теста свой робот или свой type — список фильтруется по ним.
Тестовые события помечены details.testRun, тревоги — id test-al-*; в конце прогона их удаляет journal_cleanup.
"""

from __future__ import annotations

import time
from urllib.parse import quote

import grpc
import pytest

import fleet_database_pb2 as db
from conftest import Journal, Missions, error_code, unique
from test_missions import T, iso
from test_rest import get

ALARM_FIELDS = {"id", "severity", "source", "code", "title", "detail", "robotId", "missionId", "active",
                "createdAt", "clearedAt", "acknowledgedAt", "acknowledgedBy"}


def page(path: str, query: str) -> tuple[list[dict], str | None]:
    status, body = get(f"{path}?{query}")
    assert status == 200, body
    assert set(body) == {"items", "nextCursor"}
    return body["items"], body["nextCursor"]


def all_pages(path: str, query: str) -> list[list[dict]]:
    pages, cursor = [], None
    while True:
        items, cursor = page(path, query + (f"&before={quote(cursor)}" if cursor else ""))
        pages.append(items)
        if cursor is None:
            return pages
        assert len(pages) < 100, "cursor does not advance"


def events_of_robot(robot_id: str, extra: str = "") -> list[dict]:
    items, _ = page("/api/fms/events", f"robotId={robot_id}&limit=200{extra}")
    return items


# ------------------------------------------------------------------ AppendEvents → GET /events

def test_event_in_contract_format(journal):
    """Событие для UI — поля строки (id, at, source, type, robotId, missionId, alarmId) + details как есть."""
    robot = unique("test-robot")
    journal.append(Journal.event("ROBOT", "CONNECTION", at_ms=T + 123, robot_id=robot, details={"state": "OFFLINE"}))

    event, = events_of_robot(robot)
    assert isinstance(event["id"], int)
    assert {k: event[k] for k in ("at", "source", "type", "robotId", "missionId", "alarmId", "state")} == {
        "at": "2026-09-21T14:13:20.123Z", "source": "ROBOT", "type": "CONNECTION", "robotId": robot,
        "missionId": None, "alarmId": None, "state": "OFFLINE"}


def test_events_newest_first_and_pages_cover_everything_once(journal):
    robot = unique("test-robot")
    journal.append(*[Journal.event("ROBOT", "AVAILABILITY", robot_id=robot, details={"n": i}) for i in range(5)])

    pages = all_pages("/api/fms/events", f"robotId={robot}&limit=2")
    assert [len(p) for p in pages] == [2, 2, 1]
    assert [e["n"] for p in pages for e in p] == [4, 3, 2, 1, 0]   # порядок записи, новые сверху


def test_filter_by_source_and_type(journal):
    robot = unique("test-robot")
    alarm_id = unique("test-al")
    journal.append(Journal.event("ROBOT", "CONNECTION", robot_id=robot),
                   Journal.event("ROBOT", "ERROR", robot_id=robot),
                   Journal.event("ALARM", "RAISED", robot_id=robot, alarm_id=alarm_id))

    assert [e["type"] for e in events_of_robot(robot, "&source=ALARM")] == ["RAISED"]
    assert [e["type"] for e in events_of_robot(robot, "&source=ROBOT,ALARM")] == ["RAISED", "ERROR", "CONNECTION"]
    assert [e["type"] for e in events_of_robot(robot, "&type=CONNECTION,ERROR")] == ["ERROR", "CONNECTION"]
    items, _ = page("/api/fms/events", f"alarmId={alarm_id}")
    assert [(e["type"], e["alarmId"]) for e in items] == [("RAISED", alarm_id)]


def test_system_event_without_robot(journal):
    """SYSTEM — без робота; найти его можно по type."""
    kind = unique("TEST_SYSTEM").upper()
    journal.append(Journal.event("SYSTEM", kind, details={"mapId": "map-x", "version": 3}))
    event, = page("/api/fms/events", f"source=SYSTEM&type={kind}")[0]
    assert (event["robotId"], event["mapId"], event["version"]) == (None, "map-x", 3)


def test_filter_by_period(journal):
    """[from, to) по времени события, как у миссий."""
    robot = unique("test-robot")
    journal.append(*[Journal.event("ROBOT", "AVAILABILITY", at_ms=T + d, robot_id=robot, details={"d": d})
                     for d in (-1000, 0, 1000, 2000)])
    period = f"&from={iso(T)}&to={iso(T + 2000)}"
    assert [e["d"] for e in events_of_robot(robot, period)] == [1000, 0]


def test_event_about_a_mission_is_not_in_the_mission_history(journal, missions):
    """Событие робота или тревоги с missionId видно в журнале по миссии, но не в её истории (7.3):
    там только события самой миссии, они приходят с SaveMission."""
    mission = missions.save(events=[Missions.event("STATE", {"state": "QUEUED"})])
    journal.append(Journal.event("ALARM", "RAISED", mission_id=mission.mission_id, details={"code": "X"}))

    items, _ = page("/api/fms/events", f"missionId={mission.mission_id}")
    assert sorted(e["source"] for e in items) == ["ALARM", "MISSION"]
    status, history = get(f"/api/fms/missions/{mission.mission_id}/events")
    assert [e["type"] for e in history["items"]] == ["STATE"]


def test_row_fields_win_over_details(journal):
    robot = unique("test-robot")
    journal.append(Journal.event("ROBOT", "ERROR", robot_id=robot,
                                 details={"id": "fake", "source": "X", "type": "Y", "robotId": "other", "at": "never"}))
    event, = events_of_robot(robot)
    assert isinstance(event["id"], int)
    assert (event["source"], event["type"], event["robotId"]) == ("ROBOT", "ERROR", robot)


def no_at() -> db.Event:
    event = Journal.event("ROBOT", "ERROR")
    event.ClearField("at")
    return event


@pytest.mark.parametrize("bad", [
    Journal.event("MISSION", "STATE"),          # события миссий — только с миссией
    Journal.event("", "STATE"),
    Journal.event("ROBOT", ""),
    no_at(),
    Journal.event("ROBOT", "ERROR", details="[1]"),
    Journal.event("ROBOT", "ERROR", details="{not json"),
], ids=["mission source", "no source", "no type", "no at", "details array", "details not json"])
def test_invalid_event_writes_nothing(journal, bad):
    """Ошибка в одном событии — INVALID_ARGUMENT, не записано ни одно из пачки."""
    robot = unique("test-robot")
    good = Journal.event("ROBOT", "CONNECTION", robot_id=robot)
    bad.robot_id = robot
    assert error_code(journal.control.AppendEvents, db.AppendEventsRequest(events=[good, bad])) == \
        grpc.StatusCode.INVALID_ARGUMENT
    assert events_of_robot(robot) == []


def test_event_of_unknown_mission(journal):
    robot = unique("test-robot")
    event = Journal.event("ALARM", "RAISED", robot_id=robot, mission_id=unique("test-mission"))
    assert error_code(journal.control.AppendEvents, db.AppendEventsRequest(events=[event])) == \
        grpc.StatusCode.NOT_FOUND
    assert events_of_robot(robot) == []


@pytest.mark.parametrize("query, field", [
    ("source=MISSIONS", "source"),
    ("source=ROBOT,x", "source"),
    ("limit=0", "limit"),
    ("limit=201", "limit"),
    ("before=abc", "before"),
    ("from=yesterday", "from"),
    ("from=2026-11-02T00:00:00Z&to=2026-11-01T00:00:00Z", "to"),
])
def test_events_invalid_query(query, field):
    status, error = get(f"/api/fms/events?{query}")
    assert status == 400
    assert error["code"] == "INVALID_INPUT"
    assert set(error["details"]) == {field}


# ------------------------------------------------------------------ SaveAlarm → GET /alarms

def alarms_of_robot(robot_id: str, extra: str = "") -> list[dict]:
    items, _ = page("/api/fms/alarms", f"robotId={robot_id}{extra}")
    return items


def test_alarm_in_contract_format(alarms):
    alarm = alarms.save(alarms.record(created_ms=T + 5, severity="CRITICAL", code="EMERGENCY_STOP",
                                      title="Emergency stop", detail="amr: emergency stop pressed",
                                      key="EMERGENCY_STOP/x"))
    item, = alarms_of_robot(alarm.robot_id)
    assert set(item) == ALARM_FIELDS
    assert item == {
        "id": alarm.alarm_id, "severity": "CRITICAL", "source": "ROBOT", "code": "EMERGENCY_STOP",
        "title": "Emergency stop", "detail": "amr: emergency stop pressed", "robotId": alarm.robot_id,
        "missionId": None, "active": True, "createdAt": iso(T + 5), "clearedAt": None,
        "acknowledgedAt": None, "acknowledgedBy": None,
    }
    assert get(f"/api/fms/alarms/{alarm.alarm_id}") == (200, item)


def test_unknown_alarm():
    status, error = get(f"/api/fms/alarms/{unique('test-al')}")
    assert status == 404 and error["code"] == "NOT_FOUND"


def test_save_replaces_whole_alarm(alarms):
    """Причина ушла, потом подтвердили — та же строка, новые поля."""
    alarm = alarms.save(alarms.record(created_ms=T))
    alarms.save(alarms.record(created_ms=T, alarm_id=alarm.alarm_id, robot_id=alarm.robot_id, key=alarm.key,
                              active=False, cleared_ms=T + 1000, acknowledged_ms=T + 2000, acknowledged_by="u-1"))

    item, = alarms_of_robot(alarm.robot_id)
    assert (item["active"], item["clearedAt"], item["acknowledgedAt"], item["acknowledgedBy"]) == \
        (False, iso(T + 1000), iso(T + 2000), "u-1")


def test_one_active_alarm_per_key(alarms):
    """Вторая активная с тем же key — ALREADY_EXISTS. Первую сняли — новую можно."""
    now = int(time.time() * 1000)
    first = alarms.save(alarms.record(created_ms=now))
    second = alarms.record(created_ms=now + 1, robot_id=first.robot_id, key=first.key)
    assert error_code(alarms.control.SaveAlarm, db.SaveAlarmRequest(alarm=second)) == grpc.StatusCode.ALREADY_EXISTS

    # Сняли первую — время создания у неё прежнее
    alarms.save(alarms.record(created_ms=now, alarm_id=first.alarm_id, robot_id=first.robot_id, key=first.key,
                              active=False, cleared_ms=now + 5))
    alarms.save(second)
    assert [a["active"] for a in alarms_of_robot(first.robot_id)] == [True, False]


def test_alarm_events_written_with_the_alarm(alarms, journal):
    """События тревоги (RAISED) — в той же транзакции: тревога не записалась — и событий нет."""
    alarm = alarms.record()
    raised = Journal.event("ALARM", "RAISED", robot_id=alarm.robot_id, alarm_id=alarm.alarm_id,
                           details={"code": alarm.code})
    alarms.save(alarm, raised)
    assert [e["type"] for e in events_of_robot(alarm.robot_id)] == ["RAISED"]

    duplicate = alarms.record(robot_id=alarm.robot_id, key=alarm.key)
    again = Journal.event("ALARM", "RAISED", robot_id=alarm.robot_id, alarm_id=duplicate.alarm_id)
    request = db.SaveAlarmRequest(alarm=duplicate, events=[again])
    assert error_code(alarms.control.SaveAlarm, request) == grpc.StatusCode.ALREADY_EXISTS
    assert [e["alarmId"] for e in events_of_robot(alarm.robot_id)] == [alarm.alarm_id]


def test_open_alarms_are_active_or_not_acknowledged(alarms):
    now = int(time.time() * 1000)
    robot = unique("test-robot")
    active = alarms.save(alarms.record(created_ms=now, robot_id=robot, key=f"A/{robot}"))
    unacked = alarms.save(alarms.record(created_ms=now + 1, robot_id=robot, key=f"B/{robot}",
                                        active=False, cleared_ms=now + 5))
    alarms.save(alarms.record(created_ms=now + 2, robot_id=robot, key=f"C/{robot}",
                              active=False, cleared_ms=now + 5, acknowledged_ms=now + 6))

    assert [a.alarm_id for a in alarms.open()] == [active.alarm_id, unacked.alarm_id]
    opened = alarms.open()[0]
    assert (opened.robot_id, opened.key, opened.active, opened.HasField("cleared_at")) == (robot, f"A/{robot}", True, False)


def test_alarm_filters(alarms):
    robot = unique("test-robot")
    now = int(time.time() * 1000)
    a = alarms.save(alarms.record(created_ms=now, robot_id=robot, key=f"A/{robot}", severity="WARNING"))
    b = alarms.save(alarms.record(created_ms=now + 1, robot_id=robot, key=f"B/{robot}", severity="CRITICAL",
                                  active=False, cleared_ms=now + 5))
    c = alarms.save(alarms.record(created_ms=now + 2, robot_id=robot, key=f"C/{robot}", severity="ERROR",
                                  acknowledged_ms=now + 6))

    ids = lambda extra: [x["id"] for x in alarms_of_robot(robot, extra)]   # noqa: E731
    assert ids("") == [c.alarm_id, b.alarm_id, a.alarm_id]                 # новые сверху
    assert ids("&active=true") == [c.alarm_id, a.alarm_id]
    assert ids("&acknowledged=false") == [b.alarm_id, a.alarm_id]
    assert ids("&severity=ERROR,CRITICAL") == [c.alarm_id, b.alarm_id]


def test_alarm_pages(alarms):
    robot = unique("test-robot")
    now = int(time.time() * 1000)
    saved = [alarms.save(alarms.record(created_ms=now, robot_id=robot, key=f"K{i}/{robot}")) for i in range(5)]
    pages = all_pages("/api/fms/alarms", f"robotId={robot}&limit=2")
    assert [len(p) for p in pages] == [2, 2, 1]
    assert [a["id"] for p in pages for a in p] == sorted((a.alarm_id for a in saved), reverse=True)


def cleared_but_active(alarms):
    alarm = alarms.record(active=True)
    alarm.cleared_at.FromMilliseconds(T)
    return alarm


@pytest.mark.parametrize("fields", [
    {"severity": "LOUD"},
    {"source": "ROBOTS"},
    {"code": ""},
    {"key": ""},
    {"title": ""},
], ids=["severity", "source", "no code", "no key", "no title"])
def test_invalid_alarm(alarms, fields):
    alarm = alarms.record(**fields)
    assert error_code(alarms.control.SaveAlarm, db.SaveAlarmRequest(alarm=alarm)) == grpc.StatusCode.INVALID_ARGUMENT
    assert get(f"/api/fms/alarms/{alarm.alarm_id}")[0] == 404


def test_active_alarm_has_no_cleared_at(alarms):
    alarm = cleared_but_active(alarms)
    assert error_code(alarms.control.SaveAlarm, db.SaveAlarmRequest(alarm=alarm)) == grpc.StatusCode.INVALID_ARGUMENT


@pytest.mark.parametrize("query, field", [
    ("active=yes", "active"),
    ("acknowledged=1", "acknowledged"),
    ("severity=LOUD", "severity"),
    ("limit=0", "limit"),
    ("before=abc", "before"),
])
def test_alarms_invalid_query(query, field):
    status, error = get(f"/api/fms/alarms?{query}")
    assert status == 400
    assert error["code"] == "INVALID_INPUT"
    assert set(error["details"]) == {field}
