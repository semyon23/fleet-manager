"""Журнал и тревоги в control (M-5b): события роботов и системы, тревоги и их подтверждение.

Журнал и тревоги читаем из базы (GET /events, /alarms — то, что видит UI), подтверждаем через control
(POST /alarms/{id}/ack). Тестовые роботы без симулятора на связь не выходят — у них через 5 с поднимается
ROBOT_OFFLINE; это и проверяем. Тесты с роботами симулятора — те же, что в test_mission_run.py.
"""

from __future__ import annotations

import time
from urllib.parse import urlencode

from conftest import CONTROL, DATABASE_HTTP, SIM_MAP, http
from test_availability import wait_availability
from test_mission_run import run_map, two_robots  # noqa: F401 — фикстуры с роботами симулятора

OFFLINE_WAIT = 15   # задержка тревоги 5 с + шаг таймера + выход статуса из dispatch


def events(**query) -> list[dict]:
    """Журнал из базы, новые сверху."""
    code, page = http("GET", f"{DATABASE_HTTP}/api/fms/events?{urlencode({'limit': 200, **query})}")
    assert code == 200, page
    return page["items"]


def alarms(**query) -> list[dict]:
    code, page = http("GET", f"{DATABASE_HTTP}/api/fms/alarms?{urlencode(query)}")
    assert code == 200, page
    return page["items"]


def ack(alarm_id: str):
    return http("POST", f"{CONTROL}/api/fms/alarms/{alarm_id}/ack")


def wait_for(read, check, timeout: float, what: str):
    deadline = time.monotonic() + timeout
    value = None
    while time.monotonic() < deadline:
        value = read()
        if check(value):
            return value
        time.sleep(0.3)
    raise TimeoutError(f"no {what} after {timeout:.0f} s: {value}")


def types(items: list[dict]) -> list[str]:
    return [e["type"] for e in items]


def offline_alarm(robot_id: str) -> dict:
    """Дождаться активной ROBOT_OFFLINE робота."""
    active = wait_for(lambda: alarms(robotId=robot_id, active="true"), lambda a: len(a) == 1, OFFLINE_WAIT,
                      f"ROBOT_OFFLINE of {robot_id}")
    return active[0]


# ------------------------------------------------------------------ события действий оператора

def test_registered_and_deleted_robot_in_journal(robots):
    code, robot = robots.register()
    assert code == 201, robot
    registered = wait_for(lambda: events(robotId=robot["id"], type="REGISTERED"), lambda e: len(e) == 1, 5,
                          "REGISTERED")[0]
    assert (registered["source"], registered["manufacturer"], registered["serialNumber"]) == \
        ("ROBOT", robot["manufacturer"], robot["serialNumber"])

    code, _ = robots.delete(robot["id"])
    assert code == 202
    deleted = wait_for(lambda: events(robotId=robot["id"], type="DELETED"), lambda e: len(e) == 1, 5, "DELETED")[0]
    assert (deleted["force"], deleted["canceledMission"]) == (False, None)


def test_map_saved_and_deleted_in_journal(maps):
    code, created = maps.create()
    assert code == 201, created

    def of_map():
        return [e for e in events(source="SYSTEM", type="MAP_SAVED,MAP_DELETED") if e.get("mapId") == created["id"]]

    saved, = wait_for(of_map, lambda e: len(e) == 1, 5, "MAP_SAVED")
    assert (saved["type"], saved["version"], saved["forced"], saved["robotId"]) == ("MAP_SAVED", 1, False, None)

    code, _ = maps.delete(created["id"])
    assert code == 204
    assert types(wait_for(of_map, lambda e: len(e) == 2, 5, "MAP_DELETED")) == ["MAP_DELETED", "MAP_SAVED"]


# ------------------------------------------------------------------ тревоги

def test_robot_that_never_comes_online_raises_offline_alarm(robots):
    """Зарегистрировали, а робот не выходит на связь — через 5 с ROBOT_OFFLINE, в журнале RAISED."""
    _, robot = robots.register()
    alarm = offline_alarm(robot["id"])

    assert (alarm["code"], alarm["severity"], alarm["source"], alarm["active"]) == ("ROBOT_OFFLINE", "ERROR", "ROBOT", True)
    assert alarm["detail"].startswith(f"{robot['id']}: connection ")
    assert alarm["acknowledgedAt"] is None
    raised, = wait_for(lambda: events(alarmId=alarm["id"]), lambda e: len(e) == 1, 5, "RAISED")
    assert (raised["source"], raised["type"], raised["code"], raised["robotId"]) == \
        ("ALARM", "RAISED", "ROBOT_OFFLINE", robot["id"])


def test_ack_alarm(robots):
    """Подтверждение — тревога остаётся активной (причина не ушла), но подтверждённой; повтор — то же."""
    _, robot = robots.register()
    alarm = offline_alarm(robot["id"])

    code, acked = ack(alarm["id"])
    assert code == 200, acked
    assert (acked["id"], acked["active"]) == (alarm["id"], True)
    assert acked["acknowledgedAt"] and acked["acknowledgedBy"] is None
    assert set(acked) == set(alarm)                                       # та же форма, что у базы

    code, again = ack(alarm["id"])
    assert code == 200 and again["acknowledgedAt"] == acked["acknowledgedAt"]

    in_database = wait_for(lambda: alarms(robotId=robot["id"]), lambda a: a and a[0]["acknowledgedAt"], 5, "ack in db")
    assert in_database[0]["acknowledgedAt"] == acked["acknowledgedAt"]
    assert types(wait_for(lambda: events(alarmId=alarm["id"]), lambda e: len(e) == 2, 5, "ACKED")) == ["ACKED", "RAISED"]


def test_deleted_robot_alarms_are_closed(robots):
    """Робота удалили — его тревоги сняты и подтверждены fms: действовать с ними больше некому."""
    _, robot = robots.register()
    alarm = offline_alarm(robot["id"])
    code, _ = robots.delete(robot["id"])
    assert code == 202

    closed = wait_for(lambda: alarms(robotId=robot["id"]), lambda a: a and a[0]["acknowledgedAt"], 5, "closed alarm")[0]
    assert (closed["active"], closed["acknowledgedBy"]) == (False, "fms")
    assert closed["clearedAt"]
    history = wait_for(lambda: events(alarmId=alarm["id"]), lambda e: len(e) == 3, 5, "CLEARED and ACKED")
    assert types(history) == ["ACKED", "CLEARED", "RAISED"]
    assert history[1]["reason"] == "robot removed from fms"
    assert ack(alarm["id"])[0] == 404                                    # закрыта — подтверждать нечего


def test_ack_unknown_alarm():
    code, error = ack("al-nope0000")
    assert code == 404
    assert error["code"] == "NOT_FOUND"


# ------------------------------------------------------------------ с роботами симулятора

def test_operator_actions_and_availability_in_journal(robots, two_robots):
    """amr1 поставили на узел (фикстура), пауза и снятие паузы — в журнале действия оператора и смена готовности.
    Робот на связи — тревоги ROBOT_OFFLINE у него нет."""
    amr1, _ = two_robots
    positions = [e for e in events(robotId=amr1, type="POSITION")]
    assert positions and (positions[0]["mapId"], positions[0]["nodeId"]) == (SIM_MAP, "a")

    code, _ = robots.command(amr1, "pause")
    assert code == 202
    wait_availability(robots, amr1, "PAUSED", timeout=10)
    code, _ = robots.command(amr1, "resume")
    assert code == 202
    wait_availability(robots, amr1, "IDLE", timeout=10)

    journal = wait_for(lambda: events(robotId=amr1, type="PAUSED,RESUMED,AVAILABILITY"),
                       lambda e: [x["type"] for x in e[:4]] == ["AVAILABILITY", "RESUMED", "AVAILABILITY", "PAUSED"],
                       5, "pause and resume")
    assert [(e["type"], e.get("state")) for e in journal[:4]] == \
        [("AVAILABILITY", "IDLE"), ("RESUMED", None), ("AVAILABILITY", "PAUSED"), ("PAUSED", None)]
    assert all(a["code"] != "ROBOT_OFFLINE" for a in alarms(robotId=amr1, active="true"))
