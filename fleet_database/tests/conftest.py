"""Настройка тестов fleet_database. Нужен только запущенный fleet_database (и PostgreSQL за ним).

Таблицу перед тестами не чистим: в ней могут быть чужие роботы. Вместо этого каждый тест создаёт
своих роботов с уникальными robot_id и serial_number (test-<случайное>), смотрит только на них
и в конце удаляет их через API. Чужие записи тесты не видят и не трогают.
"""

from __future__ import annotations

import os
import sys
import uuid
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


# ------------------------------------------------------------------ фикстуры

@pytest.fixture(scope="session")
def channel():
    address = os.environ.get("DATABASE_ADDRESS", "localhost:5002")
    channel = grpc.insecure_channel(address)
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
