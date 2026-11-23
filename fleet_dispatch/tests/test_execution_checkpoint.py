"""Durable execution lookup through real dispatch and fleet_database, on a private WSL DB."""
import os
import socket
import subprocess
import uuid
from contextlib import ExitStack

import grpc
import pytest

import fleet_database_pb2 as db
import fleet_database_pb2_grpc as db_rpc
import fleet_dispatch_pb2 as dp
import fleet_dispatch_pb2_grpc as dp_rpc
import fms_types_pb2 as fms
from dispatch_client import mission_of, step_wait
from test_dispatch_startup import ROOT, first
from test_move_restart_stand import wait
from test_recovery_probe import process

pytestmark = pytest.mark.skipif(
    os.getenv("EXECUTION_CHECKPOINT_STAND") != "1", reason="requires isolated WSL PostgreSQL",
)


def bind_port(stack):
    sock = stack.enter_context(socket.socket())
    sock.bind(("127.0.0.1", 0))
    return sock.getsockname()[1]


def claim(store, mission, attempt):
    add = dp.AddMissionRequest(mission=mission, attempt=attempt)
    response = store.DispatchAddReceipt(db.DispatchAddReceiptRequest(
        operation=db.DispatchAddReceiptRequest.CLAIM, mission_id=mission.mission_id,
        attempt=attempt, request_bytes=add.SerializeToString(),
    ), timeout=5)
    assert response.state == db.DispatchAddReceiptResponse.CLAIMED_NOW


def save(store, mission, version, state, step_state, cancel_id="", stopped=False):
    status = fms.MissionStatus(mission_id=mission.mission_id, robot_id=mission.robot_id, state=state)
    step = status.steps.add(node_id="wait", state=step_state, attempts=1)
    if state == fms.MISSION_STATE_RUNNING:
        status.current_node_id = step.node_id
    if state == fms.MISSION_STATE_FAILED:
        status.error.code = fms.MISSION_ERROR_CODE_TIMEOUT
        status.error.message = "wait timed out"
    execution = db.MissionExecution(mission=mission, status=status, checkpoint_version=version,
                                    cancel_action_id=cancel_id, stopped_after_cancel=stopped)
    store.SaveMissionExecution(db.SaveMissionExecutionRequest(execution=execution), timeout=5)
    return status


def check_head(api, mid, kind, version, status, attempt):
    head = api.GetExecutionCheckpoint(fms.GetExecutionCheckpointRequest(mission_id=mid), timeout=5)
    assert (head.mission_id, head.kind, head.checkpoint_version) == (mid, kind, version)
    assert head.HasField("add_attempt") == (attempt is not None)
    if attempt is not None:
        assert head.add_attempt == attempt
    assert head.HasField("status") == (status is not None)
    if status is not None:
        assert head.robot_id == status.robot_id
        assert head.status == status


def assert_rpc_error(call, *codes):
    with pytest.raises(grpc.RpcError) as error:
        call()
    assert error.value.code() in codes, (error.value.code(), error.value.details())


def test_lookup_through_two_dispatch_restarts(tmp_path):
    result = fms.GetExecutionCheckpointResponse
    dbname = "fms_checkpoint_" + uuid.uuid4().hex[:16]
    pg = ["sudo", "-n", "-u", "postgres"]
    build = ROOT / "cmake-build-debug"
    print(f"database={dbname} logs={tmp_path}", flush=True)
    with ExitStack() as services:
        subprocess.run([*pg, "createdb", "-O", "fms", dbname], check=True)
        services.callback(subprocess.run, [*pg, "dropdb", dbname], check=True)
        # A bound, non-listening socket keeps dispatch isolated from every MQTT broker.
        mqtt_port = bind_port(services)
        with ExitStack() as ports:
            db_addr = f"127.0.0.1:{bind_port(ports)}"
            http_addr = f"127.0.0.1:{bind_port(ports)}"
            dispatch_addr = f"127.0.0.1:{bind_port(ports)}"
        db_env = dict(os.environ, GRPC_ADDRESS=db_addr, HTTP_ADDRESS=http_addr,
                      DATABASE_URL=f"postgresql://fms:fms@127.0.0.1:5432/{dbname}",
                      MIGRATIONS_DIR=str(ROOT / "fleet_database/migrations"))
        database = services.enter_context(process(
            [build / "fleet_database/fleet_database"], tmp_path / "database.log", db_env,
        ))
        dbc = services.enter_context(grpc.insecure_channel(db_addr))
        store = db_rpc.DispatchStorageStub(dbc)
        snapshot_req = db.LoadDispatchSnapshotRequest()
        empty = store.LoadDispatchSnapshot(snapshot_req, wait_for_ready=True, timeout=30).snapshot
        assert empty.ByteSize() == 0

        registry = db_rpc.RobotRegistryStub(dbc)

        def new_mission(mid):
            rid = "r-" + mid
            registry.CreateRobot(db.CreateRobotRequest(spec=db.RobotSpec(
                robot_id=rid, manufacturer="test", serial_number=rid,
            )), timeout=5)
            return mission_of(mid, rid, [step_wait("wait", 60)])

        trip = new_mission("trip")
        claim(store, trip, 0)
        running = save(store, trip, 1, fms.MISSION_STATE_RUNNING, fms.STEP_STATE_RUNNING)
        pending = new_mission("pending")
        claim(store, pending, 4)
        failed = save(store, new_mission("failed"), 1, fms.MISSION_STATE_FAILED, fms.STEP_STATE_FAILED)
        canceled = save(store, new_mission("canceled"), 1, fms.MISSION_STATE_CANCELED,
                        fms.STEP_STATE_CANCELED, cancel_id="cancel-1", stopped=True)
        unproved = save(store, new_mission("unproved"), 1, fms.MISSION_STATE_CANCELED,
                        fms.STEP_STATE_CANCELED, cancel_id="cancel-2")

        fd_env = dict(os.environ, DISPATCH_LISTEN_ADDRESS=dispatch_addr, DATABASE_ADDRESS=db_addr,
                      MQTT_HOST="127.0.0.1", MQTT_PORT=str(mqtt_port), MQTT_USER="", MQTT_PASSWORD="")
        for epoch in range(3):
            log = tmp_path / f"dispatch-{epoch}.log"
            with process([build / "fleet_dispatch/fleet_dispatch"], log, fd_env) as dispatch, \
                    grpc.insecure_channel(dispatch_addr) as channel:
                api = dp_rpc.FleetDispatchStub(channel)

                def robot_snapshot():
                    assert dispatch.poll() is None, log.read_text()
                    return first(api.StreamRobotStatus, dp.StreamRobotStatusRequest()).snapshot

                robots = wait(robot_snapshot, "dispatch loaded its busy inventory", 20)
                assert any(r.robot_id == pending.robot_id and r.mission_id == pending.mission_id
                           for r in robots.robots)
                assert_rpc_error(lambda: first(api.StreamMissionStatus, dp.StreamMissionStatusRequest()),
                                 grpc.StatusCode.UNAVAILABLE)
                if epoch == 0:
                    check_head(api, "trip", result.ACTIVE, 1, running, 0)
                    completed = save(store, trip, 2, fms.MISSION_STATE_COMPLETED, fms.STEP_STATE_COMPLETED)
                # Completed executions disappear from restart inventory. They must remain addressable.
                inventory = store.LoadDispatchSnapshot(snapshot_req, timeout=5).snapshot
                assert "trip" not in [e.mission.mission_id for e in inventory.active_missions]
                assert "trip" not in [u.mission_id for u in inventory.unresolved]
                for reader in (store, api):
                    check_head(reader, "trip", result.CONFIRMED_TERMINAL, 2, completed, 0)
                    check_head(reader, "pending", result.UNCERTAIN, 0, None, 4)
                    check_head(reader, "failed", result.UNCERTAIN, 1, failed, None)
                    check_head(reader, "unproved", result.UNCERTAIN, 1, unproved, None)
                    check_head(reader, "canceled", result.CONFIRMED_TERMINAL, 1, canceled, None)
                    assert_rpc_error(lambda: reader.GetExecutionCheckpoint(
                        fms.GetExecutionCheckpointRequest(), timeout=5), grpc.StatusCode.INVALID_ARGUMENT)
                    assert_rpc_error(lambda: reader.GetExecutionCheckpoint(
                        fms.GetExecutionCheckpointRequest(mission_id="missing"), timeout=5),
                        grpc.StatusCode.NOT_FOUND)
                after = store.LoadDispatchSnapshot(snapshot_req, timeout=5).snapshot
                assert after.unresolved == inventory.unresolved
                assert after.active_missions == inventory.active_missions
                assert after.last_grants == inventory.last_grants
                assert any(r.robot_id == pending.robot_id and r.mission_id == pending.mission_id
                           for r in robot_snapshot().robots)
                assert_rpc_error(lambda: first(api.StreamMissionStatus, dp.StreamMissionStatusRequest()),
                                 grpc.StatusCode.UNAVAILABLE)
                if epoch == 2:
                    database.kill()
                    database.wait(timeout=10)
                    assert_rpc_error(lambda: api.GetExecutionCheckpoint(
                        fms.GetExecutionCheckpointRequest(mission_id="trip"), timeout=7),
                        grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.DEADLINE_EXCEEDED)
                    assert dispatch.poll() is None, log.read_text()
