"""Запускать на отдельном стенде.

control: DATABASE_ADDRESS=127.0.0.1:50058; реальная база:5002, dispatch:5001.
Тестовый прокси пропускает остальные RPC, блокирует только журнал команды.
"""
from concurrent.futures import ThreadPoolExecutor
import os
import json
import subprocess
import warnings
import threading
import time

import grpc
import pytest
from google.protobuf import message_factory
from conftest import CONTROL, SIM_MAP, Missions, http, db, dbgrpc, DATABASE_URL
from test_mission_run import run_map as mission_run_map, two_robots, action, wait_mission, wait_robot


class JournalProxy:
    def __init__(self):
        self.channel = grpc.insecure_channel(os.environ.get("JOURNAL_REAL_DATABASE", "127.0.0.1:5002"))
        self.storage = dbgrpc.ControlStorageStub(self.channel)
        self.lock = threading.Lock()
        self.release = threading.Event()
        self.release.set()
        self.seen = threading.Event()
        self.kind = None
        self.mode = "pass"
        self.records = {}
        self.calls = 0
        self.server = grpc.server(ThreadPoolExecutor(max_workers=24))
        for service in db.DESCRIPTOR.services_by_name.values():
            handlers = {}
            for method in service.methods:
                if method.client_streaming or method.server_streaming:
                    raise RuntimeError("database proxy expects unary RPC")
                input_type = message_factory.GetMessageClass(method.input_type)
                output_type = message_factory.GetMessageClass(method.output_type)
                remote = self.channel.unary_unary(
                    f"/{service.full_name}/{method.name}",
                    request_serializer=lambda m: m.SerializeToString(),
                    response_deserializer=output_type.FromString)
                def unary(request, context, remote=remote, method_name=method.name):
                    selected = False
                    mode = "pass"
                    if method_name == "PutControlCommand":
                        with self.lock:
                            self.records.setdefault(request.command.command_id, set()).add(
                                request.command.SerializeToString())
                            if request.command.kind == self.kind:
                                selected = True
                                mode = self.mode
                                self.calls += 1
                                if mode in {"lost", "resolved"}:
                                    self.mode = "pass"
                        if selected:
                            self.seen.set()
                            if mode == "blocked" and not self.release.is_set():
                                context.abort(grpc.StatusCode.UNAVAILABLE, "injected journal unavailable")
                    try:
                        response = remote(request, timeout=5)
                    except grpc.RpcError as error:
                        context.abort(error.code(), error.details())
                    if selected and mode == "lost":
                        context.abort(grpc.StatusCode.UNAVAILABLE, "injected lost journal acknowledgement")
                    if selected and mode == "resolved":
                        self.storage.ResolveControlCommand(db.ResolveControlCommandRequest(
                            command_id=request.command.command_id), timeout=5)
                        response.resolved = True
                    return response
                handlers[method.name] = grpc.unary_unary_rpc_method_handler(
                    unary, request_deserializer=input_type.FromString,
                    response_serializer=lambda m: m.SerializeToString())
            self.server.add_generic_rpc_handlers((
                grpc.method_handlers_generic_handler(service.full_name, handlers),))
        health = self.channel.unary_unary("/grpc.health.v1.Health/Check")
        def check_health(request, context):
            try:return health(request, timeout=3)
            except grpc.RpcError as error:context.abort(error.code(), error.details())
        self.server.add_generic_rpc_handlers((grpc.method_handlers_generic_handler(
            "grpc.health.v1.Health", {"Check":grpc.unary_unary_rpc_method_handler(check_health)}),))
        address=os.environ.get("JOURNAL_PROXY_LISTEN", "127.0.0.1:50058")
        if not self.server.add_insecure_port(address):raise RuntimeError(f"cannot listen at {address}")
        self.server.start()

    def arm(self, kind, mode):
        with self.lock:
            self.kind=kind;self.mode=mode;self.calls=0;self.records={}
        self.seen.clear()
        if mode=="blocked":self.release.clear()
        else:self.release.set()


@pytest.fixture(scope="session")
def journal_proxy():
    if os.environ.get("JOURNAL_FAULT_TESTS") != "1":
        pytest.skip("set JOURNAL_FAULT_TESTS=1; control must use database proxy:50058")
    proxy=JournalProxy()
    try:yield proxy
    finally:
        proxy.release.set()
        proxy.server.stop(2).wait()
        proxy.channel.close()


@pytest.fixture(scope="session", autouse=True)
def control_is_running(journal_proxy):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        try:
            code, body = http("GET", f"{CONTROL}/api/health")
            if code == 200 and body["status"] == "OK":
                return
        except OSError:
            pass
        time.sleep(0.25)
    pytest.fail("control cannot reach database and dispatch")


@pytest.fixture
def run_map(maps, monkeypatch, request):
    create = maps.create

    def create_when_loaded(*args, **kwargs):
        deadline = time.monotonic() + 30
        while True:
            code, body = create(*args, **kwargs)
            if not (code == 503 and body.get("message") == "maps are loading, try again later"):
                return code, body
            if time.monotonic() >= deadline:
                pytest.fail(f"map store still loading after 30s: {body}")
            time.sleep(0.25)

    monkeypatch.setattr(maps, "create", create_when_loaded)
    return request.getfixturevalue("mission_run_map")


@pytest.fixture
def barrier_robots(journal_proxy, two_robots):
    return two_robots


def create(missions, robot, seconds=0):
    step={"nodeId":"b"}
    if seconds:step["actions"]=[action(seconds)]
    code,mission=missions.create(Missions.body(SIM_MAP, requestedRobotId=robot, steps=[step]))
    assert code==201,mission
    return mission["id"]


def stationary(missions, robots, mid, robot, node, expected_state):
    deadline=time.monotonic()+3
    while time.monotonic()<deadline:
        code,mission=missions.from_database(mid)
        assert code==200 and mission["state"]==expected_state,mission
        code,body=robots.from_database(robot)
        assert code==200,body
        assert not body["status"]["driving"],body
        assert body["status"]["lastNodeId"]==node,body
        time.sleep(0.1)


@pytest.mark.parametrize("kind", [db.CONTROL_COMMAND_KIND_ADD_MISSION,
    db.CONTROL_COMMAND_KIND_EXTEND_MISSION, db.CONTROL_COMMAND_KIND_CANCEL_MISSION])
def test_no_command_before_journal_ack(missions, robots, barrier_robots, journal_proxy, kind):
    robot,_=barrier_robots
    journal_proxy.arm(kind,"blocked")
    mid=create(missions, robot, seconds=20 if kind==db.CONTROL_COMMAND_KIND_CANCEL_MISSION else 0)
    try:
        if kind==db.CONTROL_COMMAND_KIND_CANCEL_MISSION:
            wait_robot(robots,robot,lambda s:s["status"]["lastNodeId"]=="b" and
                not s["status"]["driving"],"arrival before cancellation",timeout=20)
            code,body=missions.cancel(mid)
            assert code==503,body
            code,body=missions.cancel(mid)
            assert code==503,body  # CANCELING не обходит журнал при повторе API.
        assert journal_proxy.seen.wait(30)
        state={db.CONTROL_COMMAND_KIND_ADD_MISSION:"ASSIGNED",
               db.CONTROL_COMMAND_KIND_EXTEND_MISSION:"RUNNING",
               db.CONTROL_COMMAND_KIND_CANCEL_MISSION:"CANCELING"}[kind]
        wait_mission(missions,mid,{state},timeout=5)
        stationary(missions,robots,mid,robot,"b" if kind==db.CONTROL_COMMAND_KIND_CANCEL_MISSION else "a",state)
    finally:
        journal_proxy.release.set()
    result=wait_mission(missions,mid,{"COMPLETED","FAILED","CANCELED"},timeout=40)
    expected="CANCELED" if kind==db.CONTROL_COMMAND_KIND_CANCEL_MISSION else "COMPLETED"
    assert result["state"]==expected,result
    with journal_proxy.lock:
        assert all(len(versions)==1 for versions in journal_proxy.records.values())


def test_lost_journal_ack_retries_same_bytes(missions, barrier_robots, journal_proxy):
    robot,_=barrier_robots
    journal_proxy.arm(db.CONTROL_COMMAND_KIND_ADD_MISSION,"lost")
    mid=create(missions,robot)
    result=wait_mission(missions,mid,{"COMPLETED","FAILED","CANCELED"})
    assert result["state"]=="COMPLETED",result
    with journal_proxy.lock:
        adds = [versions for versions in journal_proxy.records.values()
                if db.ControlCommandRecord.FromString(next(iter(versions))).kind
                == db.CONTROL_COMMAND_KIND_ADD_MISSION]
        assert len(adds) == 1
        assert journal_proxy.calls >= 2
        assert all(len(versions)==1 for versions in journal_proxy.records.values())


def test_resolved_journal_entry_is_not_executed(missions, robots, barrier_robots, journal_proxy):
    robot,_=barrier_robots
    journal_proxy.arm(db.CONTROL_COMMAND_KIND_ADD_MISSION,"resolved")
    mid=create(missions,robot)
    assert journal_proxy.seen.wait(30)
    wait_mission(missions,mid,{"ASSIGNED"},timeout=5)
    stationary(missions,robots,mid,robot,"a","ASSIGNED")
    code,body=missions.cancel(mid)
    assert code==202,body
    result=wait_mission(missions,mid,{"CANCELED","FAILED","COMPLETED"},timeout=10)
    assert result["state"]=="CANCELED",result
