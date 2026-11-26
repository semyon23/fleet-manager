"""Read startup recovery inventory through real dispatch with an isolated storage stub."""
import os
import socket
import threading
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager

import grpc
import pytest

from dispatch_client import mission_of, step_wait
from test_dispatch_startup import ROOT, Storage, db, db_rpc, dp, dp_rpc, first, rejected
from test_move_restart_stand import wait
from test_recovery_probe import checkpoint, free_port, process

Inventory = dp.GetRecoveryInventoryResponse


class InventoryStorage(Storage):
    def __init__(self, snapshot, rows):
        super().__init__(snapshot)
        self.rows = rows

    def LoadDispatchSnapshot(self, request, context):
        if not request.recovery_only:
            return super().LoadDispatchSnapshot(request, context)
        if not self.available.is_set():
            context.abort(grpc.StatusCode.UNAVAILABLE, "injected recovery read outage")
        if self.rows is None:
            # Older storage ignores recovery_only and returns the full startup snapshot.
            return db.LoadDispatchSnapshotResponse(snapshot=self.snapshot)
        return db.LoadDispatchSnapshotResponse(recovery=self.rows)


@contextmanager
def dispatch_instances(store):
    """Keep the same DB, MQTT port and dispatch address across process restarts."""
    with ThreadPoolExecutor(4) as pool, socket.socket() as mqtt:
        mqtt.bind(("127.0.0.1", 0))
        server = grpc.server(pool)
        db_port = server.add_insecure_port("127.0.0.1:0")
        db_rpc.add_DispatchStorageServicer_to_server(store, server)
        server.start()
        try:
            addr = f"127.0.0.1:{free_port()}"
            env = dict(os.environ, DISPATCH_LISTEN_ADDRESS=addr, DATABASE_ADDRESS=f"127.0.0.1:{db_port}",
                       MQTT_HOST="127.0.0.1", MQTT_PORT=str(mqtt.getsockname()[1]), MQTT_USER="", MQTT_PASSWORD="")

            @contextmanager
            def launch(log):
                binary = ROOT / "cmake-build-debug/fleet_dispatch/fleet_dispatch"
                store.attempted.clear()
                with process([binary], log, env), grpc.insecure_channel(addr) as channel:
                    assert store.attempted.wait(8), log.read_text()
                    grpc.channel_ready_future(channel).result(timeout=5)
                    yield dp_rpc.FleetDispatchStub(channel), channel.unary_unary("/grpc.health.v1.Health/Check")

            yield launch
        finally:
            server.stop(0).wait()


def read_inventory(api):
    return api.GetRecoveryInventory(dp.GetRecoveryInventoryRequest(), timeout=2)


def after_startup(api):
    wait(lambda: first(api.StreamRobotStatus, dp.StreamRobotStatusRequest()), "startup robot snapshot")
    return read_inventory(api)


def assert_stream_sources(api, inventory):
    robots = first(api.StreamRobotStatus, dp.StreamRobotStatusRequest())
    missions = first(api.StreamMissionStatus, dp.StreamMissionStatusRequest())
    assert robots.snapshot.instance_id == inventory.instance_id
    assert missions.snapshot.instance_id == inventory.instance_id


def expect_gate_closed(api, health, store):
    assert health(b"", timeout=2) == b"\x08\x02"  # NOT_SERVING
    rejected(first, api.StreamMissionStatus, dp.StreamMissionStatusRequest())
    add = dp.AddMissionRequest(mission=mission_of("new", "unrelated", [step_wait("wait", 5)]))
    rejected(lambda: api.AddMission(add, timeout=2))
    extend = dp.ExtendMissionRequest(mission_id="m", move_node_id="s", revision=1, released_nodes=2)
    rejected(lambda: api.ExtendMission(extend, timeout=2))
    assert not store.claims


def test_loading_empty_ready_and_restart(tmp_path):
    store = InventoryStorage(db.DispatchSnapshot(), db.RecoveryRows())
    with dispatch_instances(store) as launch:
        with launch(tmp_path / "dispatch-first.log") as (api, health):
            loading = read_inventory(api)
            assert loading.state == Inventory.LOADING
            assert loading.instance_id
            assert not loading.missions
            assert read_inventory(api) == loading
            expect_gate_closed(api, health, store)

            store.available.set()
            ready = after_startup(api)
            assert ready.state == Inventory.READY
            assert ready.instance_id == loading.instance_id
            assert_stream_sources(api, ready)
            assert ready.revision > loading.revision
            assert not ready.missions and not ready.robots and not ready.blocked_robots
            assert read_inventory(api) == ready
            assert health(b"", timeout=2) == b"\x08\x01"  # SERVING

        with launch(tmp_path / "dispatch-restarted.log") as (api, _):
            restarted = after_startup(api)
            assert restarted.state == Inventory.READY
            assert restarted.instance_id and restarted.instance_id != ready.instance_id
            assert_stream_sources(api, restarted)
            assert not restarted.missions and not restarted.robots and not restarted.blocked_robots
            assert read_inventory(api) == restarted


def test_storage_errors_after_ready(tmp_path):
    rows = db.RecoveryRows()
    store = InventoryStorage(db.DispatchSnapshot(), rows)
    store.available.set()
    with dispatch_instances(store) as launch, launch(tmp_path / "dispatch.log") as (api, _):
        ready = after_startup(api)
        assert ready.state == Inventory.READY
        store.available.clear()
        rejected(read_inventory, api)

        store.rows = None
        store.available.set()
        with pytest.raises(grpc.RpcError) as error:
            read_inventory(api)
        assert error.value.code() == grpc.StatusCode.FAILED_PRECONDITION

        store.rows = rows
        assert read_inventory(api) == ready


def test_overlapping_requests_share_storage_read(tmp_path):
    entered = threading.Event()
    release = threading.Event()
    compact_reads = []

    class PausedStorage(InventoryStorage):
        def LoadDispatchSnapshot(self, request, context):
            if request.recovery_only:
                compact_reads.append(request)
                entered.set()
                if not release.wait(3):
                    context.abort(grpc.StatusCode.DEADLINE_EXCEEDED, "storage read was not released")
            return super().LoadDispatchSnapshot(request, context)

    store = PausedStorage(db.DispatchSnapshot(), db.RecoveryRows())
    store.available.set()
    with dispatch_instances(store) as launch, launch(tmp_path / "dispatch.log") as (api, _):
        wait(lambda: first(api.StreamRobotStatus, dp.StreamRobotStatusRequest()), "startup robot snapshot")
        req = dp.GetRecoveryInventoryRequest()
        leader = api.GetRecoveryInventory.future(req, timeout=5)
        try:
            assert entered.wait(3), "first request did not reach storage"
            follower = api.GetRecoveryInventory.future(req, timeout=5)
            # Let the server service another call on this channel while both inventory RPCs are pending.
            assert first(api.StreamRobotStatus, dp.StreamRobotStatusRequest()).HasField("snapshot")
            with pytest.raises(grpc.FutureTimeoutError):
                follower.result(timeout=0.25)
        finally:
            release.set()
        view = leader.result(timeout=3)
        assert view.state == Inventory.READY
        assert follower.result(timeout=3) == view
        assert len(compact_reads) == 1


def test_durable_candidates_refresh_without_opening_gate(tmp_path):
    snapshot = checkpoint()
    snapshot.robots.add().spec.CopyFrom(db.RobotSpec(
        robot_id="other", manufacturer="K", serial_number="102", lifecycle=db.ROBOT_LIFECYCLE_ACTIVE))
    rows = db.RecoveryRows(robots=["r", "other"])
    rows.missions.add(mission_id="m", robot_id="r", add_attempt=0)
    store = InventoryStorage(snapshot, rows)
    store.available.set()
    with dispatch_instances(store) as launch, launch(tmp_path / "dispatch.log") as (api, health):
        initial = after_startup(api)
        assert initial.state == Inventory.READY
        mission, = initial.missions
        assert (mission.mission_id, mission.robot_id, mission.uncertain) == ("m", "r", False)
        assert mission.HasField("add_attempt") and mission.add_attempt == 0
        assert sorted(initial.robots) == ["other", "r"]
        assert sorted(initial.blocked_robots) == ["other", "r"]
        assert read_inventory(api) == initial
        expect_gate_closed(api, health, store)

        # A durable claim created after startup has no execution checkpoint yet.
        updated = db.RecoveryRows()
        updated.CopyFrom(rows)
        updated.missions.add(mission_id="pending", robot_id="other", add_attempt=7, uncertain=True)
        store.rows = updated
        current = read_inventory(api)
        assert current.state == Inventory.READY
        assert current.instance_id == initial.instance_id and current.revision > initial.revision
        assert len(current.missions) == 2
        by_id = {item.mission_id: item for item in current.missions}
        assert by_id["m"] == mission
        pending = by_id["pending"]
        assert (pending.robot_id, pending.uncertain) == ("other", True)
        assert pending.HasField("add_attempt") and pending.add_attempt == 7
        assert read_inventory(api) == current

        # Two open missions on one robot must both remain visible.
        duplicate = db.RecoveryRows()
        duplicate.CopyFrom(updated)
        duplicate.missions.add(mission_id="another", robot_id="other", uncertain=True)
        store.rows = duplicate
        conflict = read_inventory(api)
        assert conflict.state == Inventory.CONFLICT
        assert conflict.instance_id == current.instance_id and conflict.revision > current.revision
        assert sorted((m.mission_id, m.robot_id) for m in conflict.missions) == [
            ("another", "other"), ("m", "r"), ("pending", "other")]
        assert read_inventory(api) == conflict
        expect_gate_closed(api, health, store)


@pytest.mark.parametrize(
    "owner,conflict,state",
    [("", False, Inventory.CONFLICT), ("absent-from-registry", False, Inventory.CONFLICT),
     ("r", True, Inventory.CONFLICT), ("r", False, Inventory.READY)],
    ids=["unknown-owner", "unregistered-owner", "conflicting-storage-evidence", "unknown-attempt"],
)
def test_inventory_preserves_unresolved_candidate(tmp_path, owner, conflict, state):
    snapshot = db.DispatchSnapshot()
    snapshot.robots.add().spec.CopyFrom(db.RobotSpec(
        robot_id="r", manufacturer="K", serial_number="101", lifecycle=db.ROBOT_LIFECYCLE_ACTIVE))
    snapshot.unresolved.add(mission_id="unresolved", robot_id=owner)
    rows = db.RecoveryRows(robots=["r"])
    rows.missions.add(mission_id="unresolved", robot_id=owner, uncertain=True, conflict=conflict)
    store = InventoryStorage(snapshot, rows)
    store.available.set()
    with dispatch_instances(store) as launch, launch(tmp_path / "dispatch.log") as (api, health):
        result = after_startup(api)
        assert result.state == state
        candidate, = result.missions
        assert (candidate.mission_id, candidate.robot_id, candidate.uncertain) == ("unresolved", owner, True)
        assert not candidate.HasField("add_attempt")
        assert read_inventory(api) == result
        expect_gate_closed(api, health, store)

