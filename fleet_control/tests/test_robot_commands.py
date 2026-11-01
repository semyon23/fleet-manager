"""Роботы через control (C-2b): мягкое удаление и пауза — контракт UI, раздел 5.2.

Тесты с фикстурой sim_robot работают с роботом симулятора (вся цепочка: control → база → dispatch → робот);
без симулятора они пропускаются.
"""

from __future__ import annotations

import uuid

import fleet_dispatch_pb2 as dpb
import fms_types_pb2 as fms
from conftest import CONTROL, SIM_MAP, http


# ------------------------------------------------------------------ удаление

def test_delete_robot(robots, dispatch):
    _, robot = robots.register()
    status, deleted = robots.delete(robot["id"])

    assert status == 202
    assert deleted["id"] == robot["id"]
    assert deleted["lifecycle"] == "PENDING_DELETE"
    # dispatch ответил — удаление доведено до конца: в базе робота нет
    assert robots.from_database(robot["id"])[0] == 404


def test_deleted_robot_is_removed_from_dispatch(robots, dispatch):
    _, robot = robots.register()
    robots.delete(robot["id"])
    # dispatch робота уже не знает — повторное добавление проходит как новое
    dispatch.AddRobot(dpb.AddRobotRequest(robot_id=robot["id"], manufacturer=robot["manufacturer"],
                                          serial_number=robot["serialNumber"]))


def test_delete_unknown_robot():
    status, body = http("DELETE", f"{CONTROL}/api/fms/robots/test-ctl-no-such-robot")
    assert status == 404
    assert body["code"] == "NOT_FOUND"


def test_delete_twice(robots):
    _, robot = robots.register()
    robots.delete(robot["id"])
    assert robots.delete(robot["id"])[0] == 404


def test_robot_can_be_registered_again_after_delete(robots):
    _, robot = robots.register()
    robots.delete(robot["id"])
    status, _ = robots.register(id=robot["id"], serialNumber=robot["serialNumber"])
    assert status == 201


# ------------------------------------------------------------------ пауза

def test_pause_unknown_robot():
    for command in ("pause", "resume"):
        status, body = http("POST", f"{CONTROL}/api/fms/robots/test-ctl-no-such-robot/{command}")
        assert status == 404, command
        assert body["code"] == "NOT_FOUND"


# ------------------------------------------------------------------ с роботом симулятора

def test_registered_robot_comes_online(robots, sim_robot):
    """Вся цепочка: зарегистрировали через control → dispatch подписался → статус робота в базе."""
    # ONLINE приходит из retained connection, первый state — через секунды (а у стоящего робота — до 30 с),
    # поэтому здесь только связь; что state доходит до базы, видно в test_pause_and_resume
    status = robots.from_database(sim_robot)[1]["status"]
    assert status["connection"] == "ONLINE"


def test_pause_and_resume(robots, sim_robot):
    status, body = robots.command(sim_robot, "pause")
    assert status == 202, body
    robots.wait_status(sim_robot, lambda s: s["paused"], 5, "paused")

    status, body = robots.command(sim_robot, "resume")
    assert status == 202, body
    robots.wait_status(sim_robot, lambda s: not s["paused"], 5, "not paused")


def start_mission(robots, dispatch, robot_id: str) -> str:
    """Миссия напрямую в dispatch (миссий в control ещё нет): действие на месте на 30 с.
    Сначала робот ставится на узел графа — без этого dispatch миссию не примет."""
    dispatch.InitializePosition(dpb.InitializePositionRequest(
        robot_id=robot_id, pose=fms.Pose(x=0, y=0, theta=0, map_id=SIM_MAP), last_node_id="origin"))
    # dispatch отвечает сразу после отправки команды; что робот на узле — видно по его статусу
    robots.wait_status(robot_id, lambda s: s["lastNodeId"] == "origin", 5, "robot on node origin")
    action = fms.Action(action_type="simulatedAction", blocking_type=fms.BLOCKING_TYPE_HARD)
    action.parameters.add(key="duration").value.number_value = 30
    mission = fms.Mission(mission_id=f"m-ctl-{uuid.uuid4().hex[:8]}", robot_id=robot_id)
    mission.nodes.add(id="s1", name="action", action=fms.ActionStep(actions=[action]))
    dispatch.AddMission(dpb.AddMissionRequest(mission=mission))
    return mission.mission_id


def test_delete_robot_with_mission(robots, sim_robot, dispatch):
    mission_id = start_mission(robots, dispatch, sim_robot)
    robots.wait_status(sim_robot, lambda s: s["missionId"] == mission_id, 5, "mission in status")

    status, body = robots.delete(sim_robot)
    assert status == 409
    assert body["code"] == "ROBOT_BUSY"
    assert robots.from_database(sim_robot)[1]["lifecycle"] == "ACTIVE"   # ничего не тронуто

    status, deleted = robots.delete(sim_robot, force=True)
    assert status == 202
    assert deleted["lifecycle"] == "PENDING_DELETE"

    # миссия завершена dispatch'ем: FAILED / ROBOT_REMOVED (завершённые миссии остаются в снимке)
    stream = dispatch.StreamMissionStatus(dpb.StreamMissionStatusRequest())
    snapshot = next(stream).snapshot
    stream.cancel()
    mission = next(m for m in snapshot.missions if m.mission_id == mission_id)
    assert mission.state == fms.MISSION_STATE_FAILED
    assert mission.error.code == fms.MISSION_ERROR_CODE_ROBOT_REMOVED
