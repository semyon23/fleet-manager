"""Real dispatch process with an isolated DispatchStorage stub and no MQTT broker."""
import os
import socket
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import grpc
import pytest
import fleet_database_pb2 as db
import fleet_database_pb2_grpc as db_rpc
import fleet_dispatch_pb2 as dp
import fleet_dispatch_pb2_grpc as dp_rpc
import fms_types_pb2 as fms

ROOT = Path(__file__).resolve().parents[2]


class Storage(db_rpc.DispatchStorageServicer):
    def __init__(self, snapshot):
        self.snapshot = snapshot
        self.attempted = threading.Event()
        self.available = threading.Event()
        self.claims = []

    def LoadDispatchSnapshot(self, request, context):
        self.attempted.set()
        if not self.available.is_set():
            context.abort(grpc.StatusCode.UNAVAILABLE, "injected database outage")
        return db.LoadDispatchSnapshotResponse(snapshot=self.snapshot)

    def DispatchAddReceipt(self, request, context):
        if request.operation == db.DispatchAddReceiptRequest.CLAIM:
            self.claims.append(request.mission_id)
        if request.mission_id == "replay":
            reply = dp.AddMissionResponse(status=fms.MissionStatus(mission_id="replay", robot_id="r"))
            return db.DispatchAddReceiptResponse(state=db.DispatchAddReceiptResponse.HAS_REPLY,
                                                 reply_bytes=reply.SerializeToString())
        return db.DispatchAddReceiptResponse(state=db.DispatchAddReceiptResponse.MISSING)


def first(call, req):
    stream = call(req, timeout=1)
    try:
        return next(stream)
    finally:
        stream.cancel()


def rejected(fn, *args):
    with pytest.raises(grpc.RpcError) as error:
        fn(*args)
    assert error.value.code() == grpc.StatusCode.UNAVAILABLE, error.value.details()


@pytest.mark.parametrize("held", [False, True], ids=["empty", "busy"])
def test_startup_retries_and_gates(held, tmp_path):
    snapshot = db.DispatchSnapshot()
    if held:
        r = snapshot.robots.add().spec
        r.robot_id, r.manufacturer, r.serial_number = "r", "K", "101"
        r.lifecycle = db.ROBOT_LIFECYCLE_ACTIVE
        snapshot.unresolved.add(mission_id="m", robot_id="r")
    store = Storage(snapshot)
    with ThreadPoolExecutor(4) as pool, socket.socket() as mqtt:
        mqtt.bind(("127.0.0.1", 0))
        server = grpc.server(pool)
        db_port = server.add_insecure_port("127.0.0.1:0")
        db_rpc.add_DispatchStorageServicer_to_server(store, server)
        server.start()
        with socket.socket() as port:
            port.bind(("127.0.0.1", 0))
            addr = f"127.0.0.1:{port.getsockname()[1]}"
        env = dict(os.environ, DISPATCH_LISTEN_ADDRESS=addr, DATABASE_ADDRESS=f"127.0.0.1:{db_port}",
                   MQTT_HOST="127.0.0.1", MQTT_PORT=str(mqtt.getsockname()[1]))
        binary = ROOT / "cmake-build-debug/fleet_dispatch/fleet_dispatch"
        log = tmp_path / "dispatch.log"
        try:
            with log.open("w") as out, grpc.insecure_channel(addr) as channel:
                proc = subprocess.Popen([binary], env=env, stdout=out, stderr=subprocess.STDOUT)
                try:
                    assert store.attempted.wait(8), log.read_text()
                    client = dp_rpc.FleetDispatchStub(channel)
                    grpc.channel_ready_future(channel).result(timeout=5)
                    health = channel.unary_unary("/grpc.health.v1.Health/Check")
                    assert health(b"", timeout=2) == b"\x08\x02"  # NOT_SERVING
                    rejected(first, client.StreamMissionStatus, dp.StreamMissionStatusRequest())
                    rejected(first, client.StreamRobotStatus, dp.StreamRobotStatusRequest())
                    req = dp.AddMissionRequest(mission=fms.Mission(mission_id="new", robot_id="r"))
                    rejected(lambda: client.AddMission(req, timeout=2))
                    store.available.set()
                    deadline = time.monotonic() + 12
                    while True:
                        try:
                            robots = first(client.StreamRobotStatus, dp.StreamRobotStatusRequest())
                            break
                        except grpc.RpcError as error:
                            if error.code() != grpc.StatusCode.UNAVAILABLE or time.monotonic() > deadline:
                                pytest.fail(f"{error}\n{log.read_text()}")
                            time.sleep(0.05)
                    assert robots.HasField("snapshot")
                    if held:
                        r, = robots.snapshot.robots
                        assert (r.robot_id, r.mission_id) == ("r", "m")
                        assert r.connection == fms.CONNECTION_STATE_UNKNOWN
                        duplicate = client.AddRobot(dp.AddRobotRequest(
                            robot_id="r", manufacturer="K", serial_number="101"), timeout=2)
                        assert duplicate.status.mission_id == "m"
                        rejected(first, client.StreamMissionStatus, dp.StreamMissionStatusRequest())
                        rejected(lambda: client.AddMission(req, timeout=2))
                        req.mission.mission_id = "replay"
                        assert client.AddMission(req, timeout=2).status.mission_id == "replay"
                        assert health(b"", timeout=2) == b"\x08\x02"
                    else:
                        assert not robots.snapshot.robots
                        missions = first(client.StreamMissionStatus, dp.StreamMissionStatusRequest())
                        assert missions.HasField("snapshot") and not missions.snapshot.missions
                        assert health(b"", timeout=2) == b"\x08\x01"  # SERVING
                    assert not store.claims
                finally:
                    proc.terminate()
                    try:
                        proc.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                        pytest.fail(f"dispatch failed to stop; see {log}")
        finally:
            server.stop(0).wait()
