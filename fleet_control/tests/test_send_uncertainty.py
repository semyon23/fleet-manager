"""Fault injection для MissionRunner, только на отдельном тестовом стенде.

Перед запуском перенаправить fleet_control на dispatch localhost:50059.
Настоящий dispatch остаётся на localhost:5001 (SEND_REAL_DISPATCH меняет адрес).
Прокси существует только во время pytest. Остальные сервисы не перезапускать
вместе с control: deploy/run-stand.sh может остановить весь стенд.
"""
from concurrent.futures import ThreadPoolExecutor
import os
import subprocess
import threading
import time

import grpc
import pytest
from google.protobuf import message_factory

from conftest import dpb, dgrpc, Missions, SIM_MAP, CONTROL, http
from test_mission_run import (
    action, run_map, two_robots, wait_mission, wait_robot, wait_routes_after_restart,
)


class FaultProxy:
    def __init__(self):
        self.mode = "pass"
        self.captured = None
        self.calls = 0
        self.add_calls = {}
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
                                mission_id = request.mission.mission_id
                                self.add_calls[mission_id] = self.add_calls.get(mission_id, 0) + 1
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



@pytest.fixture
def restart_control(send_proxy):
    command = os.environ.get("CONTROL_RESTART")
    if not command:
        pytest.skip("set CONTROL_RESTART to restart only fleet_control")

    def restart():
        subprocess.run(command, shell=True, check=True, timeout=30)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                code, body = http("GET", f"{CONTROL}/api/health")
                if code == 200 and body["status"] == "OK":
                    return
            except OSError:
                pass
            time.sleep(0.2)
        pytest.fail("fleet_control did not come back after restart")

    return restart


@pytest.fixture
def slow_recovery_robots(maps, monkeypatch, request, send_proxy):
    create_map = maps.create

    def create_slow_map(*args, **kwargs):
        kwargs["edges"] = [{**edge, "maxSpeed": 0.25} for edge in kwargs["edges"]]
        return create_map(*args, **kwargs)

    monkeypatch.setattr(maps, "create", create_slow_map)
    return request.getfixturevalue("two_robots")


def create_recovery_mission(missions, robot, destination):
    code, body = missions.create(Missions.body(
        SIM_MAP, requestedRobotId=robot, steps=[{"nodeId": destination}],
    ))
    assert code == 201, body
    return body["id"]


def test_recovery_protects_moving_robot(
    missions, robots, slow_recovery_robots, send_proxy, restart_control,
):
    first, second = slow_recovery_robots
    send_proxy.arm("pass")
    first_id = create_recovery_mission(missions, first, "c")

    wait_robot(
        robots, first,
        lambda robot: robot["status"]["driving"] and robot["status"]["lastNodeId"] == "a",
        "first robot moving from a toward b", timeout=35,
    )
    restart_control()

    route = wait_routes_after_restart(first)
    assert route["missionId"] == first_id, route

    second_id = create_recovery_mission(missions, second, "b")
    wait_mission(missions, second_id, {"RUNNING"}, timeout=20)
    _, first_mission = missions.from_database(first_id)
    assert first_mission["state"] == "RUNNING", first_mission

    deadline = time.monotonic() + 65
    while time.monotonic() < deadline:
        code, first_mission = missions.from_database(first_id)
        assert code == 200, first_mission
        if first_mission["state"] == "COMPLETED":
            break
        assert first_mission["state"] == "RUNNING", first_mission

        code, second_robot = robots.from_database(second)
        assert code == 200, second_robot
        assert second_robot["status"]["lastNodeId"] == "d", second_robot
        assert not second_robot["status"]["driving"], second_robot
        time.sleep(0.1)
    else:
        pytest.fail("first mission did not complete after control restart")

    with send_proxy.lock:
        assert send_proxy.add_calls[first_id] == 1

    code, body = missions.cancel(second_id)
    assert code == 202, body
    wait_mission(missions, second_id, {"CANCELED"}, timeout=20)


def test_recovery_blocks_when_add_outcome_is_unknown(
    missions, maps, send_robots, send_proxy, restart_control,
):
    first, second = send_robots
    send_proxy.arm("before")
    first_id = create_recovery_mission(missions, first, "b")
    assert send_proxy.entered.wait(30), first_id
    wait_mission(missions, first_id, {"ASSIGNED"}, timeout=10)

    # Карта и журнал остаются в отдельной БД этого сценария.
    maps.ids.remove(SIM_MAP)
    restart_control()

    code, body = maps.delete(SIM_MAP)
    assert code == 409 and body["code"] == "TRAFFIC_ACTIVE", body

    second_id = create_recovery_mission(missions, second, "c")
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        code, first_mission = missions.from_database(first_id)
        assert code == 200 and first_mission["state"] == "ASSIGNED", first_mission
        code, second_mission = missions.from_database(second_id)
        assert code == 200 and second_mission["state"] == "QUEUED", second_mission
        with send_proxy.lock:
            assert send_proxy.add_calls[first_id] == 1
            assert second_id not in send_proxy.add_calls
        time.sleep(0.2)
