"""Настройка тестов fleet_control. Тесты работают вместо UI: вызывают REST control.

Нужны запущенные fleet_control, fleet_database (и PostgreSQL) и fleet_dispatch. Большинству тестов
брокер и симулятор не нужны: роботы регистрируются с выдуманными serialNumber, dispatch просто подписывается
на их топики. Тесты, которым нужен живой робот (фикстура sim_robot), без симулятора пропускаются.
Проверяют тесты по базе (REST и gRPC) и по dispatch (gRPC).

Таблицу роботов не чистим: у каждого теста свои роботы test-ctl-<случайное> (производитель test),
после теста они удаляются из dispatch и из базы.
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
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
import fleet_dispatch_pb2 as dpb  # noqa: E402
import fleet_dispatch_pb2_grpc as dgrpc  # noqa: E402

CONTROL = os.environ.get("CONTROL_HTTP", "http://localhost:8003")
DATABASE_HTTP = os.environ.get("DATABASE_HTTP", "http://localhost:8002")
MANUFACTURER = "test"
SIM_MANUFACTURER = os.environ.get("VDA_MANUFACTURER", "ktc")   # робот симулятора
SIM_SERIAL = os.environ.get("VDA_SERIAL", "amr1")
SIM_MAP = os.environ.get("VDA_MAP_ID", "map_1")


def http(method: str, url: str, body=None):
    """(код, тело) — тело разобрано из JSON; пустое — None. body: dict → JSON, str → как есть."""
    data = None
    if body is not None:
        data = (body if isinstance(body, str) else json.dumps(body)).encode()
    request = urllib.request.Request(url, data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            status, raw = response.status, response.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    return status, json.loads(raw) if raw else None


class Robots:
    """Роботы одного теста. Всё, что тест пытался зарегистрировать, удаляется в cleanup()."""

    def __init__(self, registry, dispatch):
        self.registry = registry
        self.dispatch = dispatch
        self.ids: list[str] = []

    def body(self, **fields) -> dict:
        """Тело регистрации с уникальными id и serialNumber; fields заменяют или добавляют поля."""
        suffix = uuid.uuid4().hex[:8]
        values = {"id": f"test-ctl-{suffix}", "manufacturer": MANUFACTURER, "serialNumber": f"sn-{suffix}"}
        values.update(fields)
        if isinstance(values.get("id"), str):
            self.ids.append(values["id"])
        return values

    def register(self, **fields):
        """POST /robots в control → (код, тело)."""
        return http("POST", f"{CONTROL}/api/fms/robots", self.body(**fields))

    def patch(self, robot_id: str, body):
        return http("PATCH", f"{CONTROL}/api/fms/robots/{robot_id}", body)

    def delete(self, robot_id: str, force: bool = False):
        query = "?force=true" if force else ""
        return http("DELETE", f"{CONTROL}/api/fms/robots/{robot_id}{query}")

    def command(self, robot_id: str, name: str):
        """pause / resume"""
        return http("POST", f"{CONTROL}/api/fms/robots/{robot_id}/{name}")

    def from_database(self, robot_id: str):
        """GET базы — то, что увидит UI."""
        return http("GET", f"{DATABASE_HTTP}/api/fms/robots/{robot_id}")

    def wait_status(self, robot_id: str, condition, timeout: float, what: str) -> dict:
        """Ждать, пока статус робота в базе (его пишет dispatch) удовлетворит condition."""
        deadline = time.monotonic() + timeout
        status = None
        while time.monotonic() < deadline:
            code, robot = self.from_database(robot_id)
            status = robot.get("status") if code == 200 else None
            if status and condition(status):
                return status
            time.sleep(0.2)
        raise TimeoutError(f"timeout {timeout:.0f}s waiting for {what}\nlast status: {status}")

    def cleanup(self) -> None:
        for robot_id in self.ids:
            try:
                self.dispatch.RemoveRobot(dpb.RemoveRobotRequest(robot_id=robot_id))
            except grpc.RpcError as e:
                if e.code() != grpc.StatusCode.NOT_FOUND:
                    raise
            try:
                self.registry.SetRobotDeleting(db.SetRobotDeletingRequest(robot_id=robot_id))
                self.registry.DeleteRobot(db.DeleteRobotRequest(robot_id=robot_id))
            except grpc.RpcError as e:
                if e.code() != grpc.StatusCode.NOT_FOUND:
                    raise


def _channel(address: str, what: str):
    channel = grpc.insecure_channel(address)
    try:
        grpc.channel_ready_future(channel).result(timeout=5)
    except grpc.FutureTimeoutError:
        pytest.exit(f"{what} is not reachable at {address} — is it running?", returncode=2)
    return channel


@pytest.fixture(scope="session")
def registry():
    channel = _channel(os.environ.get("DATABASE_ADDRESS", "localhost:5002"), "fleet_database")
    yield dbgrpc.RobotRegistryStub(channel)
    channel.close()


@pytest.fixture(scope="session")
def dispatch():
    channel = _channel(os.environ.get("DISPATCH_ADDRESS", "localhost:5001"), "fleet_dispatch")
    yield dgrpc.FleetDispatchStub(channel)
    channel.close()


@pytest.fixture(scope="session", autouse=True)
def control_is_running():
    try:
        urllib.request.urlopen(f"{CONTROL}/api/health", timeout=5).read()
    except OSError:
        pytest.exit(f"fleet_control is not reachable at {CONTROL} — is it running?", returncode=2)


@pytest.fixture
def robots(registry, dispatch):
    r = Robots(registry, dispatch)
    yield r
    r.cleanup()


@pytest.fixture
def sim_robot(robots) -> str:
    """Робот симулятора, зарегистрированный через control и ONLINE в базе. Нет симулятора — тест пропускается."""
    code, body = robots.register(manufacturer=SIM_MANUFACTURER, serialNumber=SIM_SERIAL)
    if code == 409:
        pytest.fail(f"{SIM_MANUFACTURER}/{SIM_SERIAL} is already registered under another id — delete it first: {body}")
    assert code == 201, body
    try:
        robots.wait_status(body["id"], lambda s: s["connection"] == "ONLINE", 10, "ONLINE")
    except TimeoutError:
        pytest.skip(f"robot {SIM_MANUFACTURER}/{SIM_SERIAL} is not ONLINE — is the simulator running?")
    return body["id"]
