"""Настройка тестов fleet_database. Нужен только запущенный fleet_database (и PostgreSQL за ним).

Таблицу перед тестами не чистим: в ней могут быть чужие роботы. Вместо этого каждый тест создаёт
своих роботов с уникальными robot_id и serial_number (test-<случайное>), смотрит только на них
и в конце удаляет их через API. Чужие записи тесты не видят и не трогают.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import uuid
import warnings
from pathlib import Path

import grpc
import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
GENERATED = HERE / "_gen"


def _generate_stubs() -> None:
    """Python-классы из proto/*.proto. Генерируются при каждом запуске — всегда совпадают с proto."""
    import grpc_tools
    from grpc_tools import protoc

    GENERATED.mkdir(exist_ok=True)
    well_known = Path(grpc_tools.__file__).parent / "_proto"   # google/protobuf/*.proto
    protos = sorted(str(p) for p in (REPO / "proto").glob("*.proto"))
    code = protoc.main(["protoc", f"-I{REPO / 'proto'}", f"-I{well_known}",
                        f"--python_out={GENERATED}", f"--grpc_python_out={GENERATED}", *protos])
    if code != 0:
        raise RuntimeError("protoc failed, see the output above")


_generate_stubs()
sys.path.insert(0, str(GENERATED))

import fleet_database_pb2 as db  # noqa: E402
import fleet_database_pb2_grpc as dbgrpc  # noqa: E402

MANUFACTURER = "test"


def unique(prefix: str) -> str:
    return f"{prefix}-{uuid.uuid4().hex[:8]}"


def error_code(call, request) -> grpc.StatusCode:
    """Вызвать RPC, который должен вернуть ошибку, и вернуть её код."""
    with pytest.raises(grpc.RpcError) as error:
        call(request)
    return error.value.code()


class Robots:
    """Роботы одного теста. Всё, что тест пытался создать, удаляется в cleanup()."""

    def __init__(self, registry: dbgrpc.RobotRegistryStub):
        self.registry = registry
        self.ids: list[str] = []

    def spec(self, **fields) -> db.RobotSpec:
        """RobotSpec с уникальными robot_id и serial_number; fields заменяют любые поля."""
        suffix = uuid.uuid4().hex[:8]
        values = {"robot_id": f"test-{suffix}", "manufacturer": MANUFACTURER, "serial_number": f"sn-{suffix}"}
        values.update(fields)
        self.ids.append(values["robot_id"])
        return db.RobotSpec(**values)

    def create(self, **fields) -> db.RobotSpec:
        return self.registry.CreateRobot(db.CreateRobotRequest(spec=self.spec(**fields))).spec

    def get(self, robot_id: str) -> db.RobotSpec:
        return self.registry.GetRobot(db.GetRobotRequest(robot_id=robot_id)).robot.spec

    def listed(self, include_deleting: bool = False) -> list[db.RobotSpec]:
        """ListRobots, но только роботы этого теста — чужие в таблице не мешают."""
        response = self.registry.ListRobots(db.ListRobotsRequest(include_deleting=include_deleting))
        return [r.spec for r in response.robots if r.spec.robot_id in self.ids]

    def set_deleting(self, robot_id: str) -> None:
        self.registry.SetRobotDeleting(db.SetRobotDeletingRequest(robot_id=robot_id))

    def delete(self, robot_id: str) -> None:
        self.registry.DeleteRobot(db.DeleteRobotRequest(robot_id=robot_id))

    def cleanup(self) -> None:
        for robot_id in self.ids:
            try:
                self.set_deleting(robot_id)
                self.delete(robot_id)
            except grpc.RpcError as e:
                if e.code() != grpc.StatusCode.NOT_FOUND:   # NOT_FOUND — не создан или тест уже удалил
                    raise


class Maps:
    """Карты одного теста (ControlStorage — то, что пишет control). Всё созданное удаляется в cleanup()."""

    # Документ редактора UI — как его сохраняет фронт (без полей записи: id, name, version, ...)
    BODY = {
        "width": 200, "height": 100,
        "meta": {"resolution": 0.05, "origin": [-1.0, -2.0, 0.0]},
        "waypoints": [{"id": "n1", "u": 10, "v": 20}, {"id": "n2", "u": 50, "v": 20, "theta": 1.57}],
        "edges": [{"id": "e1", "from": "n1", "to": "n2", "maxSpeed": 0.5}],
        "stations": [{"id": "st1", "u": 60, "v": 30, "kind": "charge", "interactionNodeIds": ["n2"]}],
        "zones": [],
        "assignedRobots": [],
    }

    def __init__(self, control: dbgrpc.ControlStorageStub):
        self.control = control
        self.ids: list[str] = []

    def record(self, **fields) -> db.MapRecord:
        """MapRecord с уникальным map_id и документом BODY; fields заменяют любые поля."""
        values = {"map_id": unique("test-map"), "name": "Test map", "body_json": json.dumps(self.BODY)}
        values.update(fields)
        self.ids.append(values["map_id"])
        return db.MapRecord(**values)

    def create(self, **fields) -> db.MapRecord:
        return self.control.CreateMap(db.CreateMapRequest(map=self.record(**fields))).map

    def get(self, map_id: str) -> db.MapRecord:
        return self.control.GetMap(db.GetMapRequest(map_id=map_id)).map

    def listed(self) -> list[db.MapRecord]:
        """ListMaps, но только карты этого теста."""
        return [m for m in self.control.ListMaps(db.ListMapsRequest()).maps if m.map_id in self.ids]

    def update(self, map_id: str, expected_version: int, new_image: str | None = None, **fields) -> db.MapRecord:
        values = {"map_id": map_id, "name": "Test map", "body_json": json.dumps(self.BODY)}
        values.update(fields)
        request = db.UpdateMapRequest(map=db.MapRecord(**values), expected_version=expected_version)
        if new_image is not None:
            request.new_image = new_image
        return self.control.UpdateMap(request).map

    def delete(self, map_id: str) -> None:
        self.control.DeleteMap(db.DeleteMapRequest(map_id=map_id))

    def cleanup(self) -> None:
        for map_id in self.ids:
            try:
                self.delete(map_id)
            except grpc.RpcError as e:
                if e.code() != grpc.StatusCode.NOT_FOUND:   # NOT_FOUND — не создана или тест уже удалил
                    raise


class Missions:
    """Миссии одного теста (ControlStorage — то, что пишет control). Удалить миссию через API нельзя:
    история не удаляется. Поэтому id тестовых миссий — test-mission-<случайное>, а удаляет их
    фикстура missions_cleanup в конце прогона."""

    def __init__(self, control: dbgrpc.ControlStorageStub):
        self.control = control
        self.ids: list[str] = []

    def record(self, created_ms: int | None = None, **fields) -> db.MissionRecord:
        """MissionRecord с уникальным mission_id; created_at = updated_at = created_ms (по умолчанию — сейчас).
        body_json — маленький объект Mission контракта; fields заменяют любые поля."""
        mission_id = fields.pop("mission_id", unique("test-mission"))
        values = {"mission_id": mission_id, "map_id": "test-map-missions", "state": "QUEUED", "priority": 1,
                  "body_json": json.dumps({"id": mission_id, "name": "Test mission", "steps": [{"nodeId": "n1"}]})}
        values.update(fields)
        record = db.MissionRecord(**values)
        ms = int(time.time() * 1000) if created_ms is None else created_ms
        record.created_at.FromMilliseconds(ms)
        record.updated_at.FromMilliseconds(ms)
        self.ids.append(mission_id)
        return record

    def save(self, created_ms: int | None = None, events=(), **fields) -> db.MissionRecord:
        """SaveMission; events — события миссии (c3), пишутся с ней одной транзакцией."""
        record = self.record(created_ms, **fields)
        self.control.SaveMission(db.SaveMissionRequest(mission=record, events=list(events)))
        return record

    @staticmethod
    def event(type: str, details: dict | str | None = None, at_ms: int | None = None,
              robot_id: str = "") -> db.MissionEvent:
        """MissionEvent; details — dict (в JSON) или строка как есть; at — по умолчанию сейчас."""
        details_json = details if isinstance(details, str) else json.dumps(details or {})
        event = db.MissionEvent(type=type, robot_id=robot_id, details_json=details_json)
        event.at.FromMilliseconds(int(time.time() * 1000) if at_ms is None else at_ms)
        return event

    def get(self, mission_id: str) -> db.MissionRecord:
        return self.control.GetMission(db.GetMissionRequest(mission_id=mission_id)).mission

    def active(self) -> list[db.MissionRecord]:
        """ListActiveMissions, но только миссии этого теста — в порядке ответа."""
        response = self.control.ListActiveMissions(db.ListActiveMissionsRequest())
        return [m for m in response.missions if m.mission_id in self.ids]


# ------------------------------------------------------------------ фикстуры

# Клиент принимает ответы до 64 МБ (по умолчанию в gRPC — 4 МБ): так тест карты на 40 МБ
# упирается в лимит сервера (32 МБ), а не в свой.
MAX_MESSAGE = 64 * 1024 * 1024


@pytest.fixture(scope="session")
def channel():
    address = os.environ.get("DATABASE_ADDRESS", "localhost:5002")
    channel = grpc.insecure_channel(address, options=[("grpc.max_receive_message_length", MAX_MESSAGE),
                                                      ("grpc.max_send_message_length", MAX_MESSAGE)])
    try:
        grpc.channel_ready_future(channel).result(timeout=5)
    except grpc.FutureTimeoutError:
        pytest.exit(f"fleet_database is not reachable at {address} — is it running?", returncode=2)
    yield channel
    channel.close()


@pytest.fixture(scope="session")
def registry(channel):
    return dbgrpc.RobotRegistryStub(channel)


@pytest.fixture(scope="session")
def storage(channel):
    """DispatchStorage — то, что пишет dispatch. Тесты здесь работают вместо него."""
    return dbgrpc.DispatchStorageStub(channel)


@pytest.fixture
def robots(registry):
    r = Robots(registry)
    yield r
    r.cleanup()


@pytest.fixture(scope="session")
def control(channel):
    """ControlStorage — то, что пишет control (карты). Тесты здесь работают вместо него."""
    return dbgrpc.ControlStorageStub(channel)


@pytest.fixture
def maps(control):
    m = Maps(control)
    yield m
    m.cleanup()


DATABASE_URL = os.environ.get("DATABASE_URL", "postgresql://fms:fms@127.0.0.1:5432/fms")


@pytest.fixture(scope="session")
def missions_cleanup():
    """В конце прогона — удалить тестовые миссии через psql. Иначе control при старте загрузит
    незавершённые тестовые миссии как настоящие (ListActiveMissions). Их события (c3) удаляются вместе
    с ними — ON DELETE CASCADE."""
    yield
    sql = "DELETE FROM missions WHERE mission_id LIKE 'test-mission-%'"
    try:
        result = subprocess.run(["psql", DATABASE_URL, "-c", sql], capture_output=True, text=True, timeout=10)
        failed = result.returncode != 0 and result.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        failed = str(e)
    if failed:
        warnings.warn(f"test missions are left in the database ({failed.strip()}), delete them: psql -c \"{sql}\"")


@pytest.fixture
def missions(control, missions_cleanup):
    return Missions(control)


# ------------------------------------------------------------------ журнал и тревоги (M-5)

RUN = unique("run")   # метка прогона в details тестовых событий — по ней их удаляет journal_cleanup


class Journal:
    """События журнала (AppendEvents — то, что пишет control). Каждое помечено details.testRun."""

    def __init__(self, control: dbgrpc.ControlStorageStub):
        self.control = control

    @staticmethod
    def event(source: str, type: str, at_ms: int | None = None, robot_id: str = "", mission_id: str = "",
              alarm_id: str = "", details: dict | str | None = None) -> db.Event:
        """details — dict (+ testRun) или строка как есть (для проверок ошибок)."""
        if isinstance(details, str):
            details_json = details
        else:
            details_json = json.dumps({**(details or {}), "testRun": RUN})
        event = db.Event(source=source, type=type, robot_id=robot_id, mission_id=mission_id, alarm_id=alarm_id,
                         details_json=details_json)
        event.at.FromMilliseconds(int(time.time() * 1000) if at_ms is None else at_ms)
        return event

    def append(self, *events: db.Event) -> None:
        self.control.AppendEvents(db.AppendEventsRequest(events=list(events)))


class Alarms:
    """Тревоги одного теста (SaveAlarm — то, что пишет control). id — test-al-<случайное>."""

    def __init__(self, control: dbgrpc.ControlStorageStub):
        self.control = control
        self.ids: list[str] = []

    def record(self, created_ms: int | None = None, **fields) -> db.AlarmRecord:
        """Активная ROBOT_OFFLINE своего робота; fields заменяют любые поля."""
        alarm_id = fields.pop("alarm_id", unique("test-al"))
        robot_id = fields.get("robot_id", unique("test-robot"))
        values = {"alarm_id": alarm_id, "code": "ROBOT_OFFLINE", "severity": "ERROR", "source": "ROBOT",
                  "robot_id": robot_id, "title": "Robot offline", "detail": f"{robot_id}: no connection for 5 s",
                  "key": f"ROBOT_OFFLINE/{robot_id}", "active": True}
        values.update(fields)
        times = {k: values.pop(k) for k in ("cleared_ms", "acknowledged_ms") if k in values}
        alarm = db.AlarmRecord(**values)
        alarm.created_at.FromMilliseconds(int(time.time() * 1000) if created_ms is None else created_ms)
        if "cleared_ms" in times:
            alarm.cleared_at.FromMilliseconds(times["cleared_ms"])
        if "acknowledged_ms" in times:
            alarm.acknowledged_at.FromMilliseconds(times["acknowledged_ms"])
        if alarm_id not in self.ids:
            self.ids.append(alarm_id)
        return alarm

    def save(self, alarm: db.AlarmRecord, *events: db.Event) -> db.AlarmRecord:
        self.control.SaveAlarm(db.SaveAlarmRequest(alarm=alarm, events=list(events)))
        return alarm

    def open(self) -> list[db.AlarmRecord]:
        """ListOpenAlarms — только тревоги этого теста, в порядке ответа."""
        response = self.control.ListOpenAlarms(db.ListOpenAlarmsRequest())
        return [a for a in response.alarms if a.alarm_id in self.ids]


@pytest.fixture(scope="session")
def journal_cleanup():
    """В конце прогона — удалить тестовые события и тревоги через psql: удалить их через API нельзя (история),
    а открытые тестовые тревоги control при старте загрузил бы как настоящие (ListOpenAlarms)."""
    yield
    sql = "DELETE FROM events WHERE details ? 'testRun'; DELETE FROM alarms WHERE alarm_id LIKE 'test-al-%'"
    try:
        result = subprocess.run(["psql", DATABASE_URL, "-c", sql], capture_output=True, text=True, timeout=10)
        failed = result.returncode != 0 and result.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        failed = str(e)
    if failed:
        warnings.warn(f"test events and alarms are left in the database ({failed.strip()}), delete them: psql -c \"{sql}\"")


@pytest.fixture
def journal(control, journal_cleanup):
    return Journal(control)


@pytest.fixture
def alarms(control, journal_cleanup):
    return Alarms(control)
