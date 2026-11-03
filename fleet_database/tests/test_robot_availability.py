"""Готовность робота к работе (availability): пишет fleet_control (SaveRobotAvailability), читают все.

Тесты работают вместо control: сами записывают availability и проверяют, что её отдают gRPC и REST.
"""

from __future__ import annotations

import grpc

from conftest import db, error_code
from test_rest import get


def not_ready(*codes: str) -> db.RobotAvailability:
    availability = db.RobotAvailability(state=db.AVAILABILITY_STATE_NOT_READY)
    for code in codes:
        availability.reasons.add(code=code, message=f"{code.lower()} — объяснение для оператора")
    return availability


def save(registry, robot_id: str, availability: db.RobotAvailability) -> None:
    registry.SaveRobotAvailability(db.SaveRobotAvailabilityRequest(robot_id=robot_id, availability=availability))


def record(registry, robot_id: str) -> db.RobotRecord:
    return registry.GetRobot(db.GetRobotRequest(robot_id=robot_id)).robot


# ------------------------------------------------------------------ gRPC

def test_new_robot_has_no_availability(robots, registry):
    spec = robots.create()
    assert not record(registry, spec.robot_id).HasField("availability")


def test_saved_availability_is_returned(robots, registry):
    spec = robots.create()
    availability = not_ready("NOT_LOCALIZED", "LOW_BATTERY")
    save(registry, spec.robot_id, availability)

    assert record(registry, spec.robot_id).availability == availability

    listed = registry.ListRobots(db.ListRobotsRequest())
    assert next(r for r in listed.robots if r.spec.robot_id == spec.robot_id).availability == availability


def test_new_availability_replaces_old(robots, registry):
    spec = robots.create()
    save(registry, spec.robot_id, not_ready("NOT_LOCALIZED"))
    save(registry, spec.robot_id, db.RobotAvailability(state=db.AVAILABILITY_STATE_IDLE))

    availability = record(registry, spec.robot_id).availability
    assert availability.state == db.AVAILABILITY_STATE_IDLE
    assert list(availability.reasons) == []


def test_availability_does_not_touch_spec(robots, registry):
    spec = robots.create()
    save(registry, spec.robot_id, not_ready("NOT_LOCALIZED"))
    assert record(registry, spec.robot_id).spec == spec   # updated_at не меняется


def test_availability_of_pending_delete_robot(robots, registry):
    """Робот на удалении: control пишет ему DELETING."""
    spec = robots.create()
    robots.set_deleting(spec.robot_id)
    save(registry, spec.robot_id, db.RobotAvailability(state=db.AVAILABILITY_STATE_DELETING))
    assert record(registry, spec.robot_id).availability.state == db.AVAILABILITY_STATE_DELETING


def test_availability_of_unknown_robot(registry):
    request = db.SaveRobotAvailabilityRequest(robot_id="test-no-such-robot",
                                              availability=db.RobotAvailability(state=db.AVAILABILITY_STATE_IDLE))
    assert error_code(registry.SaveRobotAvailability, request) == grpc.StatusCode.NOT_FOUND


def test_availability_requires_robot_id_and_state(robots, registry):
    spec = robots.create()
    no_id = db.SaveRobotAvailabilityRequest(availability=db.RobotAvailability(state=db.AVAILABILITY_STATE_IDLE))
    no_state = db.SaveRobotAvailabilityRequest(robot_id=spec.robot_id, availability=db.RobotAvailability())
    assert error_code(registry.SaveRobotAvailability, no_id) == grpc.StatusCode.INVALID_ARGUMENT
    assert error_code(registry.SaveRobotAvailability, no_state) == grpc.StatusCode.INVALID_ARGUMENT


def test_availability_is_deleted_with_robot(robots, registry):
    spec = robots.create()
    save(registry, spec.robot_id, not_ready("NOT_LOCALIZED"))
    robots.set_deleting(spec.robot_id)
    robots.delete(spec.robot_id)
    robots.create(robot_id=spec.robot_id, serial_number=spec.serial_number)
    assert not record(registry, spec.robot_id).HasField("availability")


# ------------------------------------------------------------------ REST (контракт UI, раздел 5.1)

def test_rest_without_availability_is_null(robots):
    spec = robots.create()
    assert get(f"/api/fms/robots/{spec.robot_id}")[1]["availability"] is None


def test_rest_availability_in_contract_format(robots, registry):
    spec = robots.create()
    save(registry, spec.robot_id, not_ready("NOT_LOCALIZED"))

    availability = get(f"/api/fms/robots/{spec.robot_id}")[1]["availability"]
    assert availability == {
        "state": "NOT_READY",                                   # без префикса AVAILABILITY_STATE_
        "reasons": [{"code": "NOT_LOCALIZED", "message": "not_localized — объяснение для оператора"}],
    }


def test_rest_reasons_are_empty_list(robots, registry):
    spec = robots.create()
    save(registry, spec.robot_id, db.RobotAvailability(state=db.AVAILABILITY_STATE_IDLE))
    assert get(f"/api/fms/robots/{spec.robot_id}")[1]["availability"] == {"state": "IDLE", "reasons": []}


def test_rest_list_has_availability(robots, registry):
    spec = robots.create()
    save(registry, spec.robot_id, db.RobotAvailability(state=db.AVAILABILITY_STATE_BUSY))
    listed = {r["id"]: r for r in get("/api/fms/robots")[1]}
    assert listed[spec.robot_id]["availability"]["state"] == "BUSY"
