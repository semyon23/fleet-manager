"""ExtendMission (T1): base шага выдаётся частями, остальное — horizon; продление — VDA order update.

Маршрут во всех тестах — по прямой: origin (0) → a (3 м) → b (6 м) → c (9 м).
С released_nodes = 2 робот получает base origin → a и встаёт на a, b и c — horizon.
"""

from __future__ import annotations

import time

import grpc
import pytest

import fleet_dispatch_pb2 as dpb
import fms_types_pb2 as fms
from conftest import ORIGIN
from dispatch_client import is_final, mission_status, move_mission, new_id, node

ROUTE_METERS = 9.0


def route():
    return [node(ORIGIN, 0.0), node("a", 3.0), node("b", 6.0), node("c", 9.0)]


def extend(robot, mission_id: str, revision: int, released_nodes: int | None = None, step: str = "s1",
           replace_tail=None):
    request = dpb.ExtendMissionRequest(mission_id=mission_id, move_node_id=step, revision=revision)
    if released_nodes is not None:
        request.released_nodes = released_nodes
    if replace_tail is not None:
        request.replace_tail.CopyFrom(replace_tail)
    return robot.dispatch.ExtendMission(request)


def expect_error(code, call, *args, **kwargs):
    with pytest.raises(grpc.RpcError) as error:
        call(*args, **kwargs)
    assert error.value.code() == code, error.value.details()
    return error.value


def wait_final(robot, missions, mission_id: str):
    update = missions.wait(mission_status(mission_id, is_final), robot.settings.drive_timeout(ROUTE_METERS),
                           what=f"mission {mission_id} finished")
    return update.status


def wait_for_release(robot, missions, mission_id: str, at: str, meters: float):
    """Робот дошёл до конца base (узел at) и ждёт продления — по стриму dispatch."""
    def waiting(status) -> bool:
        move = status.steps[0].move
        return move.waiting_for_release and move.last_node_id == at
    update = missions.wait(mission_status(mission_id, waiting), robot.settings.drive_timeout(meters),
                           what=f"mission {mission_id} waiting for release at {at}")
    return update.status.steps[0].move


def start_waiting_at_a(robot, missions) -> str:
    """Миссия с base origin → a; вернуть её id, когда робот встал на a."""
    mission_id = new_id("extend")
    response = robot.add_mission(move_mission(mission_id, robot.robot_id, route(), released_nodes=2))
    assert response.status.steps[0].move.released_nodes == 2
    wait_for_release(robot, missions, mission_id, at="a", meters=3)
    return mission_id


# ------------------------------------------------------------------ движение

def test_robot_waits_at_end_of_base_then_continues(robot):
    missions = robot.mission_updates()
    mission_id = start_waiting_at_a(robot, missions)

    # Робот на самом деле стоит на a, а b и c у него — horizon
    state = robot.mqtt.latest("state")
    assert state["lastNodeId"] == "a" and not state["driving"], state
    assert [(n["nodeId"], n["released"]) for n in state["nodeStates"]] == [("b", False), ("c", False)]

    # Стоит и дальше: без продления не трогается
    time.sleep(robot.settings.t(2))
    assert robot.mqtt.latest("state")["lastNodeId"] == "a"

    since = robot.mqtt.mark()
    response = extend(robot, mission_id, revision=1, released_nodes=4)
    assert response.progress.revision == 1
    assert response.progress.released_nodes == 4
    assert response.progress.total_nodes == 4

    # Робот принял order update: тот же order, orderUpdateId 1, всё released
    robot.mqtt.wait_state(lambda s: s["orderUpdateId"] == 1 and all(n["released"] for n in s["nodeStates"]),
                          robot.settings.t(5), since=since, what="order update 1 accepted")

    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_COMPLETED, status
    move = status.steps[0].move
    assert move.last_node_id == "c" and move.last_node_index == 3, move
    assert move.revision == 1 and move.confirmed_revision == 1, move
    assert move.update_rejected == "", move
    assert not move.waiting_for_release


def test_extend_while_driving_does_not_stop_robot(robot):
    """Продления приходят, пока робот едет к концу base, — он не останавливается."""
    missions = robot.mission_updates()
    mission_id = new_id("extend-driving")
    robot.add_mission(move_mission(mission_id, robot.robot_id, route(), released_nodes=2))

    extend(robot, mission_id, revision=1, released_nodes=3)
    response = extend(robot, mission_id, revision=2, released_nodes=4)
    assert response.progress.revision == 2 and response.progress.released_nodes == 4

    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_COMPLETED, status
    assert status.steps[0].move.confirmed_revision == 2
    waited = [u for u in missions.items()
              if u.HasField("status") and u.status.mission_id == mission_id and u.status.steps[0].move.waiting_for_release]
    assert not waited, "robot stopped at the end of base although it was extended in time"
    assert robot.mqtt.latest("state")["orderUpdateId"] == 2


def test_only_start_node_released_robot_stays(robot):
    """released_nodes = 1: order у робота, но ехать некуда, пока control не продлит."""
    missions = robot.mission_updates()
    mission_id = new_id("extend-start")
    robot.add_mission(move_mission(mission_id, robot.robot_id, route(), released_nodes=1))

    wait_for_release(robot, missions, mission_id, at=ORIGIN, meters=0)
    time.sleep(robot.settings.t(2))
    state = robot.mqtt.latest("state")
    assert state["lastNodeId"] == ORIGIN and not state["driving"], state

    extend(robot, mission_id, revision=1, released_nodes=4)
    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_COMPLETED, status
    assert status.steps[0].move.last_node_id == "c"


def test_cancel_while_waiting_for_release(robot):
    missions = robot.mission_updates()
    mission_id = start_waiting_at_a(robot, missions)

    robot.cancel_mission(mission_id)
    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_CANCELED, status
    robot.wait_idle(robot.settings.t(5))   # horizon тоже сброшен: у робота нет order'а
    expect_error(grpc.StatusCode.NOT_FOUND, extend, robot, mission_id, revision=1, released_nodes=4)


# ------------------------------------------------------------------ номера правок и проверки

def test_revisions_and_invalid_requests(robot):
    missions = robot.mission_updates()
    mission_id = start_waiting_at_a(robot, missions)

    # Первая правка — только до b: робот проедет a → b и снова встанет
    extend(robot, mission_id, revision=1, released_nodes=3)

    # Повтор той же правки (control не дождался ответа) — OK, второго update роботу нет
    again = extend(robot, mission_id, revision=1, released_nodes=3)
    assert again.progress.revision == 1 and again.progress.released_nodes == 3
    # Та же правка с другими данными
    expect_error(grpc.StatusCode.ALREADY_EXISTS, extend, robot, mission_id, revision=1, released_nodes=4)
    # Нумерация с 1 и без пропусков
    expect_error(grpc.StatusCode.INVALID_ARGUMENT, extend, robot, mission_id, revision=0, released_nodes=3)
    expect_error(grpc.StatusCode.INVALID_ARGUMENT, extend, robot, mission_id, revision=3, released_nodes=4)
    # Base не отзывается и не длиннее маршрута
    expect_error(grpc.StatusCode.INVALID_ARGUMENT, extend, robot, mission_id, revision=2, released_nodes=2)
    expect_error(grpc.StatusCode.INVALID_ARGUMENT, extend, robot, mission_id, revision=2, released_nodes=5)
    # Не тот шаг, неизвестная миссия, замена хвоста
    expect_error(grpc.StatusCode.FAILED_PRECONDITION, extend, robot, mission_id, revision=2, released_nodes=4,
                 step="no-such-step")
    expect_error(grpc.StatusCode.NOT_FOUND, extend, robot, "no-such-mission", revision=1, released_nodes=2)
    tail = dpb.RouteTail(from_index=2)
    expect_error(grpc.StatusCode.UNIMPLEMENTED, extend, robot, mission_id, revision=2, replace_tail=tail)

    # Робот встал на b — на конце новой base
    move = wait_for_release(robot, missions, mission_id, at="b", meters=3)
    assert move.confirmed_revision == 1, move
    assert robot.mqtt.latest("state")["orderUpdateId"] == 1, "repeated revision must not send another update"

    extend(robot, mission_id, revision=2, released_nodes=4)
    # Правка старше последней применённой
    expect_error(grpc.StatusCode.FAILED_PRECONDITION, extend, robot, mission_id, revision=1, released_nodes=3)

    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_COMPLETED, status
    assert status.steps[0].move.revision == 2 and status.steps[0].move.confirmed_revision == 2
