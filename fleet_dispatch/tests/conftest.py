"""Настройка тестов dispatch. Всё задаётся переменными окружения (см. README.md).

Нужны: запущенный fleet_dispatch, симулятор и MQTT-брокер, к которому подключены оба.
Тесты работают вместо fleet_control: вызывают gRPC dispatch и смотрят его стримы.
Кроме того, они видят state робота напрямую из брокера — чтобы проверять, что робот
на самом деле сделал то, что сообщает dispatch.
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
sys.path.insert(0, str(REPO / "tests"))   # FmsClient и помощники из тестов симулятора

import fleet_dispatch_pb2 as dpb  # noqa: E402
import fleet_dispatch_pb2_grpc as dgrpc  # noqa: E402
import fms_types_pb2 as fms  # noqa: E402
from action_helpers import send_instant_actions  # noqa: E402
from dispatch_client import UpdateStream, robot_status  # noqa: E402
from fms_client import FmsClient, RobotTarget, is_idle  # noqa: E402


def env(name: str, default: str | None = None) -> str | None:
    return os.environ.get(name, default)


ORIGIN = "origin"   # узел, на который тест ставит робота перед началом: (0, 0)


class Settings:
    def __init__(self) -> None:
        self.dispatch_address = env("DISPATCH_ADDRESS", "localhost:5001")
        self.map_id = env("VDA_MAP_ID", "map_1")
        self.second_serial = env("VDA_SERIAL2", "amr2")
        self.timeout_scale = float(env("VDA_TIMEOUT_SCALE", "1.0"))
        self.seconds_per_meter = float(env("VDA_SECONDS_PER_METER", "2.0"))
        self.target = RobotTarget(
            host=env("VDA_BROKER_HOST", "localhost"),
            port=int(env("VDA_BROKER_PORT", "1883")),
            interface=env("VDA_INTERFACE", "amr"),
            major=env("VDA_MAJOR", "v3"),
            manufacturer=env("VDA_MANUFACTURER", "ktc"),
            serial=env("VDA_SERIAL", "amr1"),
            username=env("VDA_USER"),
            password=env("VDA_PASSWORD"),
        )

    def t(self, seconds: float) -> float:
        return seconds * self.timeout_scale

    def drive_timeout(self, meters: float) -> float:
        return self.t(10.0 + meters * self.seconds_per_meter)


class Robot:
    """Робот симулятора, зарегистрированный в dispatch под id test-<serial>."""

    def __init__(self, dispatch, settings: Settings, mqtt: FmsClient, serial: str):
        self.dispatch = dispatch
        self.settings = settings
        self.mqtt = mqtt
        self.serial = serial
        self.robot_id = f"test-{serial}"
        self.missions: list[str] = []          # запущенные тестом — отменить в конце
        self._streams: list[UpdateStream] = []

    # ------------------------------------------------------------------ dispatch

    def add(self):
        return self.dispatch.AddRobot(dpb.AddRobotRequest(
            robot_id=self.robot_id, manufacturer=self.settings.target.manufacturer, serial_number=self.serial))

    def remove(self) -> bool:
        """False — робота в dispatch и не было."""
        try:
            self.dispatch.RemoveRobot(dpb.RemoveRobotRequest(robot_id=self.robot_id))
            return True
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.NOT_FOUND:
                return False
            raise

    def add_mission(self, mission: fms.Mission):
        self.missions.append(mission.mission_id)
        return self.dispatch.AddMission(dpb.AddMissionRequest(mission=mission))

    def cancel_mission(self, mission_id: str):
        return self.dispatch.CancelMission(dpb.CancelMissionRequest(mission_id=mission_id))

    def initialize_position(self, node_id: str, x: float, y: float = 0.0, theta: float = 0.0):
        pose = fms.Pose(x=x, y=y, theta=theta, map_id=self.settings.map_id)
        return self.dispatch.InitializePosition(
            dpb.InitializePositionRequest(robot_id=self.robot_id, pose=pose, last_node_id=node_id))

    def robot_updates(self, robot_ids: list[str] | None = None) -> UpdateStream:
        """Стрим статусов роботов. По умолчанию — только этого робота."""
        request = dpb.StreamRobotStatusRequest(robot_ids=[self.robot_id] if robot_ids is None else robot_ids)
        stream = UpdateStream(self.dispatch.StreamRobotStatus(request))
        self._streams.append(stream)
        return stream

    def mission_updates(self) -> UpdateStream:
        stream = UpdateStream(self.dispatch.StreamMissionStatus(
            dpb.StreamMissionStatusRequest(robot_ids=[self.robot_id])))
        self._streams.append(stream)
        return stream

    # ----------------------------------------------------------- робот напрямую

    def wait_idle(self, timeout: float) -> dict:
        """Робот (по его собственному state) без order'а и стоит."""
        since = max(0, self.mqtt.mark() - 1)
        return self.mqtt.wait_state(lambda s: is_idle(s) and not s["driving"], timeout, since=since,
                                    what=f"robot {self.serial} idle and standing")

    def request_state(self) -> None:
        """Попросить робота прислать state сейчас (не ждать 30 с)."""
        send_instant_actions(self.mqtt, [{"actionId": f"stateRequest-{uuid.uuid4().hex[:8]}",
                                          "actionType": "stateRequest", "blockingType": "NONE"}])

    # ------------------------------------------------------------- подготовка

    def prepare(self) -> None:
        """Робот добавлен в dispatch, ONLINE и стоит на узле ORIGIN в (0, 0)."""
        self.remove()                                   # остатки прошлого прогона
        self.wait_idle(self.settings.drive_timeout(30))
        updates = self.robot_updates()
        self.add()
        updates.wait(robot_status(self.robot_id, lambda s: s.connection == fms.CONNECTION_STATE_ONLINE),
                     self.settings.t(5), what=f"{self.robot_id} ONLINE")
        self.initialize_position(ORIGIN, 0.0, 0.0)
        updates.wait(robot_status(self.robot_id, lambda s: s.last_node_id == ORIGIN),
                     self.settings.t(5), what=f"{self.robot_id} on node {ORIGIN}")

    def cleanup(self) -> None:
        for mission_id in self.missions:
            try:
                self.cancel_mission(mission_id)
            except grpc.RpcError:
                pass   # миссия уже закончилась — это нормально
        for stream in self._streams:
            stream.close()
        self.remove()


# ------------------------------------------------------------------ фикстуры

@pytest.fixture(scope="session")
def settings() -> Settings:
    return Settings()


@pytest.fixture(scope="session")
def dispatch(settings: Settings):
    channel = grpc.insecure_channel(settings.dispatch_address)
    try:
        grpc.channel_ready_future(channel).result(timeout=5)
    except grpc.FutureTimeoutError:
        pytest.exit(f"fleet_dispatch is not reachable at {settings.dispatch_address} — is it running?",
                    returncode=2)
    yield dgrpc.FleetDispatchStub(channel)
    channel.close()


@pytest.fixture(scope="session")
def mqtt(settings: Settings):
    client = FmsClient(settings.target, client_id=f"dispatch-tests-{uuid.uuid4().hex[:6]}")
    client.connect()
    send_instant_actions(client, [{"actionId": f"stateRequest-{uuid.uuid4().hex[:8]}",
                                   "actionType": "stateRequest", "blockingType": "NONE"}])
    try:
        client.wait_message("state", lambda s: True, timeout=35.0, what="first state")
    except TimeoutError:
        pytest.exit(f"no state from robot on '{settings.target.topic('state')}' — is the simulator running "
                    f"and connected to the same broker?", returncode=2)
    yield client
    client.close()


@pytest.fixture
def robot(dispatch, settings: Settings, mqtt: FmsClient):
    """Перед тестом: робот в dispatch, ONLINE, стоит на ORIGIN. После: миссии отменены, робот удалён."""
    r = Robot(dispatch, settings, mqtt, settings.target.serial)
    r.prepare()
    yield r
    r.cleanup()


@pytest.fixture
def second_robot(dispatch, settings: Settings):
    """Второй робот симулятора, только в dispatch (его state тестам не нужен)."""
    target = RobotTarget(**{**settings.target.__dict__, "serial": settings.second_serial})
    client = FmsClient(target, client_id=f"dispatch-tests-2-{uuid.uuid4().hex[:6]}")
    client.connect()
    r = Robot(dispatch, settings, client, settings.second_serial)
    r.remove()
    updates = r.robot_updates()
    r.add()
    try:
        updates.wait(robot_status(r.robot_id, lambda s: s.connection == fms.CONNECTION_STATE_ONLINE),
                     settings.t(5), what=f"{r.robot_id} ONLINE")
    except TimeoutError:
        r.cleanup()
        client.close()
        pytest.skip(f"second robot {settings.second_serial} is not ONLINE in the simulator")
    yield r
    r.cleanup()
    client.close()
