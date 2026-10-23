"""PauseRobot / ResumeRobot / InitializePosition — instant actions через dispatch."""

from __future__ import annotations

import grpc
import pytest

import fleet_dispatch_pb2 as dpb
import fms_types_pb2 as fms
from conftest import ORIGIN
from dispatch_client import move_mission, new_id, node, robot_status


def start_long_drive(robot, meters: float = 15.0):
    """Миссия «ехать прямо meters метров»; вернуть, когда робот поехал."""
    updates = robot.robot_updates()
    mission = move_mission(new_id("drive"), robot.robot_id, [node(ORIGIN, 0.0), node("far", meters)])
    robot.add_mission(mission)
    updates.wait(robot_status(robot.robot_id, lambda s: s.driving), robot.settings.t(5), what="robot driving")
    return updates


def test_pause_and_resume(robot):
    updates = start_long_drive(robot)

    robot.dispatch.PauseRobot(dpb.PauseRobotRequest(robot_id=robot.robot_id))
    updates.wait(robot_status(robot.robot_id, lambda s: s.paused and not s.driving),
                 robot.settings.t(5), what="paused and standing")

    robot.dispatch.ResumeRobot(dpb.ResumeRobotRequest(robot_id=robot.robot_id))
    updates.wait(robot_status(robot.robot_id, lambda s: not s.paused and s.driving),
                 robot.settings.t(5), what="resumed and driving")


def test_pause_unknown_robot(dispatch):
    with pytest.raises(grpc.RpcError) as error:
        dispatch.PauseRobot(dpb.PauseRobotRequest(robot_id="test-no-such-robot"))
    assert error.value.code() == grpc.StatusCode.NOT_FOUND


def test_initialize_position_puts_robot_on_node(robot):
    updates = robot.robot_updates()
    robot.initialize_position("p5", 5.0, 1.0)
    status = updates.wait(robot_status(robot.robot_id, lambda s: s.last_node_id == "p5"),
                          robot.settings.t(5), what="last_node_id p5").status
    assert status.pose.x == pytest.approx(5.0, abs=0.01)
    assert status.pose.y == pytest.approx(1.0, abs=0.01)


def test_initialize_position_requires_node_and_map(robot):
    with pytest.raises(grpc.RpcError) as error:
        robot.dispatch.InitializePosition(dpb.InitializePositionRequest(robot_id=robot.robot_id))
    assert error.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def action_ids_in_errors(status) -> set[str]:
    return {e.references["actionId"] for e in status.errors if "actionId" in e.references}


def test_initialize_position_is_rejected_while_driving(robot):
    updates = start_long_drive(robot)
    mark = updates.mark()
    # предупреждения прошлых тестов робот может ещё держать — новой считаем ошибку с новым actionId
    statuses = [u.status for u in updates.items() if u.HasField("status")]
    old = action_ids_in_errors(statuses[-1]) if statuses else set()

    robot.initialize_position("nowhere", 50.0, 50.0)   # ответ — «отправлено», отказ робота виден в статусе
    updates.wait(robot_status(robot.robot_id, lambda s: any(
                     "initializePosition" in action_id for action_id in action_ids_in_errors(s) - old)),
                 robot.settings.t(5), since=mark, what="robot error about our initializePosition")
    assert all(u.status.last_node_id != "nowhere" for u in updates.items(mark) if u.HasField("status")), \
        "robot must not be moved to another node while it drives an order"
