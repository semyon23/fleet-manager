"""DispatchStorage: статус робота и factsheet, которые пишет dispatch, и их чтение через RobotRegistry."""

from __future__ import annotations

import json

import grpc

import fms_types_pb2 as fms
from conftest import db, error_code


def full_status(robot_id: str) -> fms.RobotStatus:
    """Статус, где заполнено почти всё: после записи в базу и чтения он должен совпасть до поля."""
    status = fms.RobotStatus(
        robot_id=robot_id,
        connection=fms.CONNECTION_STATE_ONLINE,
        operating_mode=fms.OPERATING_MODE_AUTOMATIC,
        pose=fms.Pose(x=1.25, y=-3.5, theta=0.785, map_id="map_1"),
        localized=True,
        last_node_id="n7",
        driving=True,
        power=fms.PowerSupply(state_of_charge=87.5, charging=False, voltage=48.1),
        safety=fms.SafetyState(emergency_stop=fms.EMERGENCY_STOP_NONE),
        mission_id="mission-1",
    )
    status.errors.add(error_type="noRoute", level=fms.ERROR_LEVEL_WARNING, description="Путь «n7 → n8» закрыт",
                      references={"nodeId": "n8"})
    status.loads.add(load_id="pallet-1", load_type="EUR")
    status.last_state_at.FromMilliseconds(1_700_000_000_123)
    status.factsheet_received_at.FromMilliseconds(1_700_000_000_000)
    status.protocol.invalid_messages = 2
    status.protocol.last_error = "state.nodeStates: missing"
    return status


def save_status(storage, status: fms.RobotStatus) -> None:
    storage.SaveRobotStatus(db.SaveRobotStatusRequest(status=status))


def save_factsheet(storage, robot_id: str, factsheet: dict, received_at_ms: int | None = None) -> None:
    request = db.SaveFactsheetRequest(robot_id=robot_id, factsheet_json=json.dumps(factsheet))
    if received_at_ms is not None:
        request.received_at.FromMilliseconds(received_at_ms)
    storage.SaveFactsheet(request)


def get_record(robots, robot_id: str) -> db.RobotRecord:
    return robots.registry.GetRobot(db.GetRobotRequest(robot_id=robot_id)).robot


FACTSHEET = {
    "headerId": 1,
    "manufacturer": "test",
    "typeSpecification": {"seriesName": "amr", "agvKinematic": "DIFF", "maxLoadMass": 500},
    "protocolFeatures": {"agvActions": [{"actionType": "pick"}, {"actionType": "drop"}]},
}


# ------------------------------------------------------------------ статус робота

def test_robot_without_status(robots):
    spec = robots.create()
    assert not get_record(robots, spec.robot_id).HasField("status")


def test_saved_status_is_returned_by_get_robot(robots, storage):
    spec = robots.create()
    status = full_status(spec.robot_id)
    save_status(storage, status)

    record = get_record(robots, spec.robot_id)
    assert record.status == status
    assert record.spec == spec   # spec статус не трогает


def test_new_status_replaces_old_completely(robots, storage):
    """Хранится последний статус целиком: полей, которых нет в новом, после записи нет."""
    spec = robots.create()
    save_status(storage, full_status(spec.robot_id))

    second = fms.RobotStatus(robot_id=spec.robot_id, connection=fms.CONNECTION_STATE_OFFLINE)
    save_status(storage, second)

    status = get_record(robots, spec.robot_id).status
    assert status == second
    assert not status.HasField("pose")
    assert list(status.errors) == []


def test_list_robots_returns_status_where_saved(robots, storage):
    with_status = robots.create()
    without_status = robots.create()
    save_status(storage, full_status(with_status.robot_id))

    response = robots.registry.ListRobots(db.ListRobotsRequest())
    records = {r.spec.robot_id: r for r in response.robots if r.spec.robot_id in robots.ids}

    assert records[with_status.robot_id].status == full_status(with_status.robot_id)
    assert not records[without_status.robot_id].HasField("status")


def test_status_of_pending_delete_robot_is_saved(robots, storage):
    """Пока робот помечен на удаление, dispatch ещё работает с ним и пишет статус."""
    spec = robots.create()
    robots.set_deleting(spec.robot_id)
    save_status(storage, full_status(spec.robot_id))
    assert get_record(robots, spec.robot_id).status == full_status(spec.robot_id)


def test_status_of_unknown_robot(storage):
    request = db.SaveRobotStatusRequest(status=fms.RobotStatus(robot_id="test-no-such-robot"))
    assert error_code(storage.SaveRobotStatus, request) == grpc.StatusCode.NOT_FOUND


def test_status_requires_robot_id(storage):
    request = db.SaveRobotStatusRequest(status=fms.RobotStatus(connection=fms.CONNECTION_STATE_ONLINE))
    assert error_code(storage.SaveRobotStatus, request) == grpc.StatusCode.INVALID_ARGUMENT


# ------------------------------------------------------------------ factsheet

def test_saved_factsheet_is_returned(robots, storage):
    spec = robots.create()
    save_factsheet(storage, spec.robot_id, FACTSHEET, received_at_ms=1_700_000_000_456)

    response = robots.registry.GetFactsheet(db.GetFactsheetRequest(robot_id=spec.robot_id))
    # JSONB хранит смысл, а не текст: пробелы и порядок ключей могут отличаться — сравниваем разобранное
    assert json.loads(response.factsheet_json) == FACTSHEET
    assert response.received_at.ToMilliseconds() == 1_700_000_000_456


def test_new_factsheet_replaces_old(robots, storage):
    spec = robots.create()
    save_factsheet(storage, spec.robot_id, FACTSHEET, received_at_ms=1_700_000_000_000)
    save_factsheet(storage, spec.robot_id, {"headerId": 2}, received_at_ms=1_700_000_001_000)

    response = robots.registry.GetFactsheet(db.GetFactsheetRequest(robot_id=spec.robot_id))
    assert json.loads(response.factsheet_json) == {"headerId": 2}
    assert response.received_at.ToMilliseconds() == 1_700_000_001_000


def test_factsheet_without_received_at_gets_current_time(robots, storage):
    spec = robots.create()
    save_factsheet(storage, spec.robot_id, FACTSHEET)

    response = robots.registry.GetFactsheet(db.GetFactsheetRequest(robot_id=spec.robot_id))
    assert response.received_at.ToMilliseconds() >= spec.created_at.ToMilliseconds()


def test_robot_without_factsheet(robots):
    spec = robots.create()
    assert error_code(robots.registry.GetFactsheet, db.GetFactsheetRequest(robot_id=spec.robot_id)) \
        == grpc.StatusCode.NOT_FOUND


def test_factsheet_of_unknown_robot(storage):
    request = db.SaveFactsheetRequest(robot_id="test-no-such-robot", factsheet_json="{}")
    assert error_code(storage.SaveFactsheet, request) == grpc.StatusCode.NOT_FOUND


def test_factsheet_must_be_json(robots, storage):
    spec = robots.create()
    for bad in ("", "not json", "{\"headerId\": 1"):
        request = db.SaveFactsheetRequest(robot_id=spec.robot_id, factsheet_json=bad)
        assert error_code(storage.SaveFactsheet, request) == grpc.StatusCode.INVALID_ARGUMENT, bad


def test_factsheet_requires_robot_id(storage):
    request = db.SaveFactsheetRequest(factsheet_json="{}")
    assert error_code(storage.SaveFactsheet, request) == grpc.StatusCode.INVALID_ARGUMENT


# ------------------------------------------------------------------ удаление робота

def test_deleting_robot_deletes_its_status_and_factsheet(robots, storage):
    """Статус и factsheet удаляются вместе с роботом: тот же робот, зарегистрированный заново, — чистый."""
    spec = robots.create()
    save_status(storage, full_status(spec.robot_id))
    save_factsheet(storage, spec.robot_id, FACTSHEET)

    robots.set_deleting(spec.robot_id)
    robots.delete(spec.robot_id)
    robots.create(robot_id=spec.robot_id, serial_number=spec.serial_number)

    assert not get_record(robots, spec.robot_id).HasField("status")
    assert error_code(robots.registry.GetFactsheet, db.GetFactsheetRequest(robot_id=spec.robot_id)) \
        == grpc.StatusCode.NOT_FOUND
