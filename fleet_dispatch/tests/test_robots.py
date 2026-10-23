"""AddRobot / RemoveRobot и стрим статусов роботов."""

from __future__ import annotations

import time

import grpc
import pytest

import fleet_dispatch_pb2 as dpb
import fms_types_pb2 as fms
from conftest import ORIGIN
from dispatch_client import robot_status


def add_robot(dispatch, robot_id: str, manufacturer: str, serial: str):
    return dispatch.AddRobot(dpb.AddRobotRequest(robot_id=robot_id, manufacturer=manufacturer, serial_number=serial))


def test_repeated_add_robot_is_ok(dispatch, robot, settings):
    response = add_robot(dispatch, robot.robot_id, settings.target.manufacturer, robot.serial)
    assert response.status.robot_id == robot.robot_id
    assert response.status.connection == fms.CONNECTION_STATE_ONLINE


def test_add_robot_with_other_data_conflicts(dispatch, robot, settings):
    # тот же robot_id, другой физический робот
    with pytest.raises(grpc.RpcError) as error:
        add_robot(dispatch, robot.robot_id, settings.target.manufacturer, "no-such-serial")
    assert error.value.code() == grpc.StatusCode.ALREADY_EXISTS

    # тот же физический робот под другим robot_id
    with pytest.raises(grpc.RpcError) as error:
        add_robot(dispatch, "test-duplicate", settings.target.manufacturer, robot.serial)
    assert error.value.code() == grpc.StatusCode.ALREADY_EXISTS


def test_add_robot_requires_all_fields(dispatch):
    with pytest.raises(grpc.RpcError) as error:
        add_robot(dispatch, "test-empty", "", "")
    assert error.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_remove_unknown_robot(dispatch):
    with pytest.raises(grpc.RpcError) as error:
        dispatch.RemoveRobot(dpb.RemoveRobotRequest(robot_id="test-no-such-robot"))
    assert error.value.code() == grpc.StatusCode.NOT_FOUND


def test_stream_starts_with_snapshot(robot):
    updates = robot.robot_updates()
    first = updates.wait(lambda u: True, robot.settings.t(3), what="first message")
    assert first.HasField("snapshot"), "first message of the stream must be a snapshot"

    statuses = {s.robot_id: s for s in first.snapshot.robots}
    assert robot.robot_id in statuses
    status = statuses[robot.robot_id]
    assert status.connection == fms.CONNECTION_STATE_ONLINE
    assert status.last_node_id == ORIGIN
    assert status.HasField("factsheet_received_at"), "factsheet should have arrived (retained)"
    assert status.operating_mode == fms.OPERATING_MODE_AUTOMATIC


def test_every_state_reaches_the_stream(robot):
    updates = robot.robot_updates()
    first = updates.wait(lambda u: u.HasField("snapshot"), robot.settings.t(3), what="snapshot")
    before = first.snapshot.robots[0].last_state_at.ToNanoseconds()

    robot.request_state()
    updates.wait(robot_status(robot.robot_id, lambda s: s.last_state_at.ToNanoseconds() > before),
                 robot.settings.t(3), what="status with a newer last_state_at")


def test_remove_robot_is_reported(robot):
    updates = robot.robot_updates()
    updates.wait(lambda u: u.HasField("snapshot"), robot.settings.t(3), what="snapshot")
    assert robot.remove()
    updates.wait(lambda u: u.removed_robot_id == robot.robot_id, robot.settings.t(3),
                 what=f"removed_robot_id {robot.robot_id}")


def test_two_subscribers_get_the_same_updates(robot):
    first = robot.robot_updates()
    second = robot.robot_updates()
    for stream in (first, second):
        stream.wait(lambda u: u.HasField("snapshot"), robot.settings.t(3), what="snapshot")

    robot.request_state()
    for stream in (first, second):
        stream.wait(robot_status(robot.robot_id), robot.settings.t(3), what="status after stateRequest")


def test_filter_skips_other_robots(robot):
    updates = robot.robot_updates(robot_ids=["test-someone-else"])
    first = updates.wait(lambda u: True, robot.settings.t(3), what="first message")
    assert first.HasField("snapshot") and len(first.snapshot.robots) == 0

    robot.request_state()
    time.sleep(robot.settings.t(2))
    assert not [u for u in updates.items(1) if u.HasField("status")], "statuses of a filtered-out robot arrived"


def test_invalid_message_is_counted(robot):
    updates = robot.robot_updates()
    updates.wait(lambda u: u.HasField("snapshot"), robot.settings.t(3), what="snapshot")

    robot.mqtt.send_raw("state", "this is not json")
    status = updates.wait(robot_status(robot.robot_id, lambda s: s.protocol.invalid_messages >= 1),
                          robot.settings.t(3), what="protocol.invalid_messages >= 1")
    assert status.status.protocol.last_error, "last_error should describe the problem"

    # сессия продолжает работать: следующий нормальный state снова обрабатывается
    robot.request_state()
    updates.wait(robot_status(robot.robot_id, lambda s: s.connection == fms.CONNECTION_STATE_ONLINE),
                 robot.settings.t(3), what="status after a valid state")
