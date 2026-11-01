"""Роботы через control (C-2a): регистрация и изменение имени и меток — контракт UI, раздел 5.2."""

from __future__ import annotations

import pytest

import fleet_dispatch_pb2 as dpb
from conftest import CONTROL, MANUFACTURER, http

ROBOT_FIELDS = {"id", "name", "manufacturer", "serialNumber", "labels", "lifecycle", "createdAt", "updatedAt",
                "status", "availability", "graphPosition", "info"}


# ------------------------------------------------------------------ регистрация

def test_register_returns_robot(robots):
    status, robot = robots.register(name="Робот 1", labels={"zone": "A"})

    assert status == 201
    assert set(robot) == ROBOT_FIELDS
    assert robot["name"] == "Робот 1"
    assert robot["manufacturer"] == MANUFACTURER
    assert robot["labels"] == {"zone": "A"}
    assert robot["lifecycle"] == "ACTIVE"
    assert robot["status"] is None


def test_registered_robot_is_in_database(robots):
    _, robot = robots.register(name="r")
    status, stored = robots.from_database(robot["id"])
    assert status == 200
    stored.pop("status")          # dispatch мог уже записать статус (UNKNOWN) — сравниваем остальное
    robot.pop("status")
    assert stored == robot


def test_registered_robot_is_added_to_dispatch(robots, dispatch):
    _, robot = robots.register()
    # RemoveRobot без NOT_FOUND — значит, dispatch робота знает
    dispatch.RemoveRobot(dpb.RemoveRobotRequest(robot_id=robot["id"]))


def test_name_and_labels_are_optional(robots):
    status, robot = robots.register()
    assert status == 201
    assert robot["name"] == robot["id"]        # по контракту: по умолчанию = id
    assert robot["labels"] == {}


@pytest.mark.parametrize("fields, field", [
    ({"id": None}, "id"),
    ({"id": ""}, "id"),
    ({"id": "with space"}, "id"),
    ({"id": "кириллица"}, "id"),
    ({"id": "x" * 65}, "id"),
    ({"id": 42}, "id"),
    ({"manufacturer": ""}, "manufacturer"),
    ({"serialNumber": None}, "serialNumber"),
    ({"name": 1}, "name"),
    ({"labels": ["a"]}, "labels"),
    ({"labels": {"zone": 1}}, "labels"),
])
def test_invalid_field(robots, fields, field):
    status, body = robots.register(**fields)
    assert status == 400
    assert body["code"] == "INVALID_INPUT"
    assert field in body["details"], body


def test_id_of_64_allowed_characters(robots):
    status, robot = robots.register(id="A-z_0-9-" + "x" * 56)
    assert status == 201, robot


def test_all_field_errors_at_once(robots):
    status, body = robots.register(id="bad id", manufacturer="", serialNumber="")
    assert status == 400
    assert set(body["details"]) == {"id", "manufacturer", "serialNumber"}


@pytest.mark.parametrize("raw", ["not json", "[1, 2]", ""])
def test_body_must_be_json_object(raw):
    status, body = http("POST", f"{CONTROL}/api/fms/robots", raw)
    assert status == 400
    assert body["code"] == "INVALID_INPUT"


def test_same_id_conflicts(robots):
    _, robot = robots.register()
    status, body = robots.register(id=robot["id"])
    assert status == 409
    assert body["code"] == "ALREADY_EXISTS"


def test_same_physical_robot_conflicts(robots):
    _, robot = robots.register()
    status, body = robots.register(serialNumber=robot["serialNumber"])
    assert status == 409
    assert body["code"] == "ALREADY_EXISTS"


# ------------------------------------------------------------------ PATCH

def test_patch_name_and_labels(robots):
    _, robot = robots.register(name="old", labels={"zone": "A"})
    status, patched = robots.patch(robot["id"], {"name": "new", "labels": {"zone": "B"}})

    assert status == 200
    assert patched["name"] == "new"
    assert patched["labels"] == {"zone": "B"}
    assert patched["createdAt"] == robot["createdAt"]
    assert robots.from_database(robot["id"])[1]["name"] == "new"


def test_patch_only_name_keeps_labels(robots):
    _, robot = robots.register(name="old", labels={"zone": "A"})
    _, patched = robots.patch(robot["id"], {"name": "new"})
    assert patched["labels"] == {"zone": "A"}


def test_patch_only_labels_keeps_name(robots):
    _, robot = robots.register(name="old", labels={"zone": "A"})
    _, patched = robots.patch(robot["id"], {"labels": {}})
    assert patched["name"] == "old"
    assert patched["labels"] == {}


def test_patch_null_name_resets_to_id(robots):
    _, robot = robots.register(name="old")
    _, patched = robots.patch(robot["id"], {"name": None})
    assert patched["name"] == robot["id"]


@pytest.mark.parametrize("field, value", [("id", "other"), ("manufacturer", "other"), ("serialNumber", "other")])
def test_patch_cannot_change_identity(robots, field, value):
    _, robot = robots.register()
    status, body = robots.patch(robot["id"], {field: value})
    assert status == 400
    assert field in body["details"]


def test_patch_invalid_labels(robots):
    _, robot = robots.register()
    status, body = robots.patch(robot["id"], {"labels": "zone=A"})
    assert status == 400
    assert "labels" in body["details"]


def test_patch_unknown_robot():
    status, body = http("PATCH", f"{CONTROL}/api/fms/robots/test-ctl-no-such-robot", {"name": "x"})
    assert status == 404
    assert body["code"] == "NOT_FOUND"
