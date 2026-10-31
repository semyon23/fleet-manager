"""Общие помощники для тестов action'ов: отправка instantActions, ожидание статусов,
пауза, проверки «держится всё время»."""

from __future__ import annotations

import json
import time
import uuid

def action(action_type: str, params: dict | None = None, action_id: str | None = None,
           blocking: str = "NONE") -> dict:
    a = {"actionId": action_id or f"{action_type}-{uuid.uuid4().hex[:8]}",
         "actionType": action_type, "blockingType": blocking}
    if params:
        a["actionParameters"] = [{"key": k, "value": v} for k, v in params.items()]
    return a


def send_instant(robot, *actions: dict) -> int:
    """Отправить instantActions; возвращает отметку журнала state до отправки."""
    mark = robot.fms.mark()
    send_instant_actions(robot.fms, list(actions))
    return mark


def send_instant_actions(fms, actions: list[dict]) -> None:
    """Сам файл не зависит от версии fms_client.py: заголовок — тем же счётчиком, что у order."""
    msg = {**fms._header("instantActions"), "actions": actions}
    fms.send_raw("instantActions", json.dumps(msg))


def status_of(state: dict, action_id: str, key: str = "instantActionStates") -> str | None:
    for a in state.get(key) or []:
        if a.get("actionId") == action_id:
            return a.get("actionStatus")
    return None


def order_action_status(state: dict, action_id: str) -> str | None:
    return status_of(state, action_id, "actionStates")


def has_error_for(state: dict, error_type: str, action_id: str) -> bool:
    """Ошибка нужного типа со ссылкой на этот actionId (а не оставшаяся от прошлого теста)."""
    for e in state.get("errors", []):
        if e.get("errorType") != error_type:
            continue
        for r in e.get("errorReferences") or []:
            if r.get("referenceKey") == "actionId" and r.get("referenceValue") == action_id:
                return True
    return False


def wait_instant(robot, a: dict, status: str, since: int, timeout: float | None = None) -> dict:
    return robot.fms.wait_state(lambda s: status_of(s, a["actionId"]) == status,
                                timeout or robot.t(3.0), since=since,
                                what=f"instant action {a['actionType']} ({a['actionId']}) {status}")


def wait_failed_with(robot, a: dict, error_type: str, since: int) -> dict:
    return robot.fms.wait_state(
        lambda s: status_of(s, a["actionId"]) == "FAILED" and has_error_for(s, error_type, a["actionId"]),
        robot.t(3.0), since=since,
        what=f"{a['actionType']} FAILED and error {error_type} referencing actionId {a['actionId']}")


def pause(robot) -> None:
    a = action("startPause")
    mark = send_instant(robot, a)
    robot.fms.wait_state(lambda s: s.get("paused") is True and status_of(s, a["actionId"]) == "FINISHED",
                         robot.t(3.0), since=mark, what="paused == true, startPause FINISHED")


def unpause(robot) -> None:
    a = action("stopPause")
    mark = send_instant(robot, a)
    robot.fms.wait_state(lambda s: not s.get("paused") and status_of(s, a["actionId"]) == "FINISHED",
                         robot.t(3.0), since=mark, what="paused == false, stopPause FINISHED")


def wait_stopped(robot, since: int) -> dict:
    return robot.fms.wait_state(lambda s: not s["driving"], robot.t(3.0), since=since,
                                what="driving == false")


def ensure_unpaused(robot) -> None:
    """Упавший на середине тест не должен оставить робота на паузе: следующие не поедут."""
    if robot.state().get("paused"):
        unpause(robot)


def wait_order_action(robot, a: dict, status: str, since: int, timeout: float) -> dict:
    return robot.fms.wait_state(lambda s: order_action_status(s, a["actionId"]) == status,
                                timeout, since=since,
                                what=f"order action {a['actionType']} ({a['actionId']}) {status}")


def describe(s: dict) -> str:
    acts = ", ".join(f"{a['actionType']}={a['actionStatus']}" for a in s.get("actionStates", []))
    return (f"headerId={s.get('headerId')} driving={s['driving']} paused={s.get('paused')} "
            f"lastNodeId={s['lastNodeId']} actions: {acts}")


def assert_holds(robot, seconds: float, predicate, what: str) -> None:
    """Как assert_stays, но в ошибке — что именно было в state."""
    since = robot.since_latest()
    time.sleep(robot.t(seconds))
    for s in robot.fms.states(since):
        assert predicate(s), f"{what}\n  {describe(s)}"


def assert_stands_while_running(robot, a: dict, since: int) -> None:
    """HARD action (6.2): пока он RUNNING, робот не едет."""
    for s in robot.fms.states(since):
        assert not (order_action_status(s, a["actionId"]) == "RUNNING" and s["driving"]), \
            f"HARD action {a['actionType']} is RUNNING while robot drives\n  {describe(s)}"
