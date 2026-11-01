"""Сверка базы с dispatch (C-3): база — источник истины, control раз в 10 с доводит dispatch до неё.

Тесты меняют базу в обход control (через её gRPC) — так, как будто control не успел довести дело до конца
или dispatch перезапустился, — и ждут, пока сверка всё исправит. Каждый тест идёт до ~15 с.
"""

from __future__ import annotations

import time

import grpc
import pytest

import fleet_database_pb2 as db
import fleet_dispatch_pb2 as dpb

SYNC_TIMEOUT = 15   # сверка раз в 10 с + запас


def wait_until(condition, timeout: float, what: str) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.5)
    raise TimeoutError(f"timeout {timeout:.0f}s waiting for {what}")


def test_robot_only_in_database_gets_to_dispatch(robots, registry):
    """Робот есть в базе, а dispatch его не знает (dispatch перезапустился или был выключен при регистрации)."""
    body = robots.body()
    registry.CreateRobot(db.CreateRobotRequest(spec=db.RobotSpec(
        robot_id=body["id"], manufacturer=body["manufacturer"], serial_number=body["serialNumber"])))
    assert robots.from_database(body["id"])[1]["status"] is None   # dispatch о нём ещё не знает

    # dispatch добавил робота — и сразу записал его первый статус в базу
    robots.wait_status(body["id"], lambda s: True, SYNC_TIMEOUT, "status written by dispatch")


def test_unfinished_delete_is_finished(robots, registry, dispatch):
    """Робот помечен PENDING_DELETE, но удаление не доведено (dispatch не ответил на DELETE)."""
    _, robot = robots.register()
    registry.SetRobotDeleting(db.SetRobotDeletingRequest(robot_id=robot["id"]))

    wait_until(lambda: robots.from_database(robot["id"])[0] == 404, SYNC_TIMEOUT, "robot deleted from database")
    with pytest.raises(grpc.RpcError) as error:
        dispatch.RemoveRobot(dpb.RemoveRobotRequest(robot_id=robot["id"]))
    assert error.value.code() == grpc.StatusCode.NOT_FOUND   # dispatch его уже отпустил
