"""REST для UI (только чтение): роботы и factsheet в формате контракта UI (docs/API_CONTRACT.md, раздел 5).

Роботов и статусы тесты пишут через gRPC (как control и dispatch), читают — через REST.
Адрес REST — DATABASE_HTTP, по умолчанию http://localhost:8002.
"""

from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.request

import fms_types_pb2 as fms
from test_dispatch_storage import FACTSHEET, full_status, save_factsheet, save_status

BASE = os.environ.get("DATABASE_HTTP", "http://localhost:8002")
ISO_MS = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$")   # время по контракту: UTC с миллисекундами

ROBOT_FIELDS = {"id", "name", "manufacturer", "serialNumber", "labels", "lifecycle", "createdAt", "updatedAt",
                "status", "availability", "graphPosition", "info"}


def request(path: str, method: str = "GET"):
    """(код, заголовки, тело) — тело разобрано из JSON; пустое — None."""
    try:
        with urllib.request.urlopen(urllib.request.Request(BASE + path, method=method), timeout=5) as response:
            status, headers, body = response.status, response.headers, response.read()
    except urllib.error.HTTPError as e:
        status, headers, body = e.code, e.headers, e.read()
    return status, headers, json.loads(body) if body else None


def get(path: str):
    status, _, body = request(path)
    return status, body


def listed(robots) -> dict:
    """GET /robots — только роботы этого теста, по id."""
    status, body = get("/api/fms/robots")
    assert status == 200
    return {robot["id"]: robot for robot in body if robot["id"] in robots.ids}


# ------------------------------------------------------------------ робот

def test_robot_in_contract_format(robots):
    spec = robots.create(name="Робот 1", labels={"zone": "A"})
    robot = listed(robots)[spec.robot_id]

    assert set(robot) == ROBOT_FIELDS
    assert robot["name"] == "Робот 1"
    assert robot["manufacturer"] == spec.manufacturer
    assert robot["serialNumber"] == spec.serial_number
    assert robot["labels"] == {"zone": "A"}
    assert robot["lifecycle"] == "ACTIVE"
    assert ISO_MS.match(robot["createdAt"]) and ISO_MS.match(robot["updatedAt"])
    assert robot["status"] is None                    # dispatch ещё ничего не писал
    assert robot["availability"] is None              # считает control — позже
    assert robot["graphPosition"] is None
    assert robot["info"] is None


def test_name_defaults_to_id(robots):
    spec = robots.create()
    assert listed(robots)[spec.robot_id]["name"] == spec.robot_id


def test_empty_labels_are_object(robots):
    spec = robots.create()
    assert listed(robots)[spec.robot_id]["labels"] == {}


def test_pending_delete_robot_is_listed(robots):
    """По контракту робот виден с PENDING_DELETE, пока удаление не закончено."""
    spec = robots.create()
    robots.set_deleting(spec.robot_id)
    assert listed(robots)[spec.robot_id]["lifecycle"] == "PENDING_DELETE"


def test_get_robot(robots):
    spec = robots.create(name="r")
    status, robot = get(f"/api/fms/robots/{spec.robot_id}")
    assert status == 200
    assert robot == listed(robots)[spec.robot_id]


def test_unknown_robot(robots):
    status, body = get("/api/fms/robots/test-no-such-robot")
    assert status == 404
    assert body["code"] == "NOT_FOUND"
    assert "test-no-such-robot" in body["message"]
    assert body["details"] == {}


# ------------------------------------------------------------------ статус

def test_status_in_contract_format(robots, storage):
    spec = robots.create()
    save_status(storage, full_status(spec.robot_id))

    status = get(f"/api/fms/robots/{spec.robot_id}")[1]["status"]
    assert status["connection"] == "ONLINE"                      # без префикса CONNECTION_STATE_
    assert status["operatingMode"] == "AUTOMATIC"
    assert status["pose"] == {"x": 1.25, "y": -3.5, "theta": 0.785, "mapId": "map_1"}
    assert status["localized"] is True
    assert status["lastNodeId"] == "n7"
    assert status["driving"] is True
    assert status["paused"] is False
    assert status["power"] == {"stateOfCharge": 87.5, "charging": False, "voltage": 48.1, "range": None}
    assert status["safety"] == {"emergencyStop": "NONE", "fieldViolation": False}
    assert status["errors"] == [{"errorType": "noRoute", "level": "WARNING", "description": "Путь «n7 → n8» закрыт",
                                 "hint": "", "references": {"nodeId": "n8"}}]
    assert status["loads"] == [{"loadId": "pallet-1", "loadType": "EUR"}]
    assert status["missionId"] == "mission-1"
    assert status["factsheetReceivedAt"] == "2023-11-14T22:13:20.000Z"
    assert status["lastStateAt"] == "2023-11-14T22:13:20.123Z"


def test_status_without_values_uses_null(robots, storage):
    """Нет значения — null, пустые коллекции — []: по общим правилам контракта."""
    spec = robots.create()
    save_status(storage, fms.RobotStatus(robot_id=spec.robot_id, connection=fms.CONNECTION_STATE_UNKNOWN))

    status = get(f"/api/fms/robots/{spec.robot_id}")[1]["status"]
    assert status["connection"] == "UNKNOWN"
    assert status["pose"] is None
    assert status["missionId"] is None
    assert status["factsheetReceivedAt"] is None
    assert status["errors"] == [] and status["loads"] == []


# ------------------------------------------------------------------ factsheet

def test_factsheet(robots, storage):
    spec = robots.create()
    save_factsheet(storage, spec.robot_id, FACTSHEET)
    status, body = get(f"/api/fms/robots/{spec.robot_id}/factsheet")
    assert status == 200
    assert body == FACTSHEET


def test_robot_without_factsheet(robots):
    spec = robots.create()
    status, body = get(f"/api/fms/robots/{spec.robot_id}/factsheet")
    assert status == 404
    assert body["code"] == "NOT_FOUND"


# ------------------------------------------------------------------ сервер

def test_unknown_path():
    status, body = get("/api/fms/no-such-path")
    assert status == 404
    assert body["code"] == "NOT_FOUND"


def test_cors_preflight():
    status, headers, body = request("/api/fms/robots", method="OPTIONS")
    assert status == 204
    assert headers["Access-Control-Allow-Origin"] == "*"
    assert body is None


def test_write_methods_are_not_here():
    """Изменения — только через control: у базы REST только для чтения."""
    status, body = request("/api/fms/robots", method="POST")[0::2]
    assert status == 404
    assert body["code"] == "NOT_FOUND"
