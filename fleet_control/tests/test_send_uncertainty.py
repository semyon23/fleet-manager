"""Fault injection для MissionRunner, только на отдельном тестовом стенде.

Перед запуском перенаправить fleet_control на dispatch localhost:50059.
Настоящий dispatch остаётся на localhost:5001 (SEND_REAL_DISPATCH меняет адрес).
Прокси существует только во время pytest. Остальные сервисы не перезапускать
вместе с control: deploy/run-stand.sh может остановить весь стенд.
"""
from concurrent.futures import ThreadPoolExecutor
import os
import threading
import time

import grpc
import pytest
from google.protobuf import message_factory

from conftest import dpb, dgrpc, Missions, SIM_MAP, CONTROL, http
from test_mission_run import run_map, two_robots, action, wait_mission


class FaultProxy:
    def __init__(self):
        self.mode = "pass"
        self.captured = None
        self.calls = 0
        self.streams = []
        self.entered = threading.Event()
        self.release = threading.Event()
        self.lock = threading.Lock()
        self.channel = grpc.insecure_channel(
            os.environ.get("SEND_REAL_DISPATCH", "localhost:5001"))
        self.stub = dgrpc.FleetDispatchStub(self.channel)
        self.server = grpc.server(ThreadPoolExecutor(max_workers=24))
        service = dpb.DESCRIPTOR.services_by_name["FleetDispatch"]
        handlers = {}
        for method in service.methods:
            name = f"/{service.full_name}/{method.name}"
            response_type = message_factory.GetMessageClass(method.output_type)
            request_type = message_factory.GetMessageClass(method.input_type)
            if method.client_streaming:
                raise RuntimeError("proxy needs adaptation for client streaming")
            if method.server_streaming:
                remote = self.channel.unary_stream(
                    name, request_serializer=lambda m: m.SerializeToString(),
                    response_deserializer=response_type.FromString)
                def stream(request, context, remote=remote):
                    call = remote(request)
                    with self.lock:
                        self.streams.append(call)
                    context.add_callback(call.cancel)
                    try:
                        yield from call
                    except grpc.RpcError as error:
                        if context.is_active():
                            context.abort(error.code(), error.details())
                handlers[method.name] = grpc.unary_stream_rpc_method_handler(
                    stream, request_deserializer=request_type.FromString,
                    response_serializer=lambda m: m.SerializeToString())
            else:
                remote = self.channel.unary_unary(
                    name, request_serializer=lambda m: m.SerializeToString(),
                    response_deserializer=response_type.FromString)
                def unary(request, context, remote=remote, method_name=method.name):
                    try:
                        if method_name == "AddMission":
                            with self.lock:
                                self.calls += 1
                                mode = self.mode
                                self.mode = "pass"
                                self.captured = request
                            self.entered.set()
                            if mode == "before":
                                context.abort(grpc.StatusCode.UNAVAILABLE, "injected before delivery")
                            if mode == "delayed":
                                if not self.release.wait(10):
                                    context.abort(grpc.StatusCode.DEADLINE_EXCEEDED, "test release missing")
                            result = remote(request, timeout=10)
                            if mode in ("lost", "delayed"):
                                context.abort(grpc.StatusCode.UNAVAILABLE, "injected lost reply")
                            return result
                        return remote(request, timeout=10)
                    except grpc.RpcError as error:
                        context.abort(error.code(), error.details())
                handlers[method.name] = grpc.unary_unary_rpc_method_handler(
                    unary, request_deserializer=request_type.FromString,
                    response_serializer=lambda m: m.SerializeToString())
        self.server.add_generic_rpc_handlers((
            grpc.method_handlers_generic_handler(service.full_name, handlers),))
        health = self.channel.unary_unary("/grpc.health.v1.Health/Check")
        def check_health(request, context):
            try:
                return health(request, timeout=3)
            except grpc.RpcError as error:
                context.abort(error.code(), error.details())
        self.server.add_generic_rpc_handlers((
            grpc.method_handlers_generic_handler("grpc.health.v1.Health", {
                "Check": grpc.unary_unary_rpc_method_handler(check_health)
            }),))
        address = os.environ.get("SEND_PROXY_LISTEN", "127.0.0.1:50059")
        if not self.server.add_insecure_port(address):
            raise RuntimeError(f"cannot listen at {address}")
        self.server.start()

    def arm(self, mode):
        with self.lock:
            self.mode = mode
            self.calls = 0
            self.captured = None
        self.entered.clear()
        self.release.clear()


@pytest.fixture(scope="session")
def send_proxy():
    if os.environ.get("SEND_FAULT_TESTS") != "1":
        pytest.skip("requires isolated control routed to fault proxy; set SEND_FAULT_TESTS=1")
    proxy = FaultProxy()
    try:
        yield proxy
    finally:
        proxy.release.set()
        proxy.server.stop(2).wait()
        proxy.channel.close()


@pytest.fixture(scope="session", autouse=True)
def control_is_running(send_proxy):
    code, body = http("GET", f"{CONTROL}/api/health")
    assert code == 200, body


@pytest.fixture
def send_robots(send_proxy, two_robots):
    # Зависимость гарантирует старт прокси до регистрации тестовых роботов.
    return two_robots


def create(missions, robot):
    code, mission = missions.create(Missions.body(
        SIM_MAP, requestedRobotId=robot,
        steps=[{"nodeId": "b", "actions": [action(5)]}]))
    assert code == 201, mission
    return mission["id"]


def assert_bound(missions, mission_id, robot, duration=3):
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        code, mission = missions.from_database(mission_id)
        assert code == 200, mission
        assert mission["robotId"] == robot, mission
        assert mission["state"] not in {"QUEUED", "FAILED", "CANCELED"}, mission
        time.sleep(0.1)


def test_lost_reply_does_not_requeue(missions, send_robots, send_proxy):
    robot, _ = send_robots
    send_proxy.arm("lost")
    mission_id = create(missions, robot)
    assert send_proxy.entered.wait(5)
    assert_bound(missions, mission_id, robot)
    result = wait_mission(missions, mission_id, {"COMPLETED", "FAILED", "CANCELED"})
    assert result["state"] == "COMPLETED", result
    assert send_proxy.calls == 1


def test_not_delivered_is_not_automatically_resent(missions, send_robots, send_proxy):
    robot, _ = send_robots
    send_proxy.arm("before")
    mission_id = create(missions, robot)
    assert send_proxy.entered.wait(5)
    try:
        assert_bound(missions, mission_id, robot)
        assert send_proxy.calls == 1
        # Новый снимок также не доказывает, что задержанный запрос не придёт.
        with send_proxy.lock:
            streams = list(send_proxy.streams)
        for stream in streams:
            stream.cancel()
        assert_bound(missions, mission_id, robot, duration=5)
    finally:
        # Тест сам разрешает неопределённость: это НЕ автоматический retry control.
        assert send_proxy.captured is not None
        send_proxy.stub.AddMission(send_proxy.captured, timeout=10)
        result = wait_mission(missions, mission_id, {"COMPLETED", "FAILED", "CANCELED"})
        assert result["state"] == "COMPLETED", result


def test_cancel_not_found_does_not_allow_late_execution(missions, send_robots, send_proxy):
    robot, _ = send_robots
    send_proxy.arm("delayed")
    mission_id = create(missions, robot)
    assert send_proxy.entered.wait(5)
    try:
        code, body = missions.cancel(mission_id)
        assert code == 202, body
        result = wait_mission(missions, mission_id, {"CANCELING", "CANCELED", "COMPLETED"}, timeout=3)
        assert result["state"] == "CANCELING", result
    finally:
        send_proxy.release.set()
    result = wait_mission(missions, mission_id, {"CANCELED", "FAILED", "COMPLETED"})
    assert result["state"] == "CANCELED", result
    assert send_proxy.calls == 1
