"""Issue a NEW mission while another owner is recovering on a private WSL stand."""
import json
import os
import signal
import threading
import time
from contextlib import ExitStack

import grpc
import pytest

import fleet_database_pb2 as db
import fleet_dispatch_pb2 as dp
import fms_types_pb2 as fms
from test_control_checkpoint_stand import Proxy
from test_independent_motion_stand import BASES, MotionStand
from test_move_restart_stand import stop, wait


pytestmark = pytest.mark.skipif(os.getenv("RECOVERY_ASSIGNMENT_STAND") != "1",
                                reason="private two-robot WSL stand")
LEAD, TRIP = BASES["A"], BASES["D"]
ADD = db.CONTROL_COMMAND_KIND_ADD_MISSION
EXTEND = db.CONTROL_COMMAND_KIND_EXTEND_MISSION


class AdmissionProxy(Proxy):
    """Forward real RPCs; hold a chosen DB reply AFTER the upstream commit."""
    def __init__(self, stack, upstream):
        self.block_inventory = threading.Event()
        self.block_compact_snapshot = threading.Event()
        self.hide_lead_checkpoint = threading.Event()
        self.barrier = None
        self.persisted = threading.Event()
        self.release = threading.Event()
        self.acknowledged = threading.Event()
        self.issued = []
        super().__init__(stack, upstream)
        stack.callback(self.release.set)

    def record_command(self, name, raw):
        if name in ("AddMissionDuringRecovery", "ExtendMissionDuringRecovery"):
            wrapper = (dp.RecoveryAddRequest if name.startswith("Add") else dp.RecoveryExtendRequest).FromString(raw)
            command = wrapper.command
            mid = command.mission.mission_id if name.startswith("Add") else command.mission_id
            if mid == TRIP:
                with self.lock:
                    self.issued.append((name, wrapper.scope, command.SerializeToString()))
        elif name in ("AddMission", "ExtendMission"):
            cmd = (dp.AddMissionRequest if name == "AddMission" else dp.ExtendMissionRequest).FromString(raw)
            if (cmd.mission.mission_id if name == "AddMission" else cmd.mission_id) == TRIP:
                with self.lock:
                    self.issued.append((name, None, cmd.SerializeToString()))

    def selected(self, name, raw):
        if not self.barrier or self.barrier[0] != name:
            return False
        if name == "PutControlCommand":
            row = db.PutControlCommandRequest.FromString(raw).command
            return row.mission_id == TRIP and row.kind == self.barrier[1]
        execution = db.SaveMissionExecutionRequest.FromString(raw).execution
        return (execution.mission.mission_id == TRIP and execution.HasField("order") and
                execution.order.order_update_id == self.barrier[1] and bool(execution.order.sent_order_json))

    def service(self, details):
        method = details.method
        name = method.rsplit("/", 1)[-1]
        if name in {"StreamRobotStatus", "StreamMissionStatus", "Watch"}:
            return super().service(details)

        def unary(raw, context):
            if (name == "LoadDispatchSnapshot" and self.block_compact_snapshot.is_set() and
                    db.LoadDispatchSnapshotRequest.FromString(raw).recovery_only):
                self.hit("compact read failed")
                context.abort(grpc.StatusCode.UNAVAILABLE, "stand: dispatch cannot refresh its inventory")
            if name == "GetRecoveryInventory" and self.block_inventory.is_set():
                self.hit("denied inventory")
                context.abort(grpc.StatusCode.UNAVAILABLE, "stand: recovery view unavailable")
            if name == "GetExecutionCheckpoint" and self.hide_lead_checkpoint.is_set():
                if fms.GetExecutionCheckpointRequest.FromString(raw).mission_id == LEAD:
                    self.hit("unknown A route")
                    context.abort(grpc.StatusCode.UNAVAILABLE, "stand: A grant cannot be verified")
            self.record_command(name, raw)
            try:
                reply = self.channel.unary_unary(method)(raw, timeout=3)
            except grpc.RpcError as error:
                self.hit(name + " " + error.code().name)
                context.abort(error.code(), error.details())
            if self.selected(name, raw):
                self.persisted.set()
                while not self.release.wait(.02):
                    if not context.is_active():
                        context.abort(grpc.StatusCode.CANCELLED, "client deadline before release")
                if context.is_active():
                    self.acknowledged.set()
            return reply
        return grpc.unary_unary_rpc_method_handler(unary)


class AssignmentStand(MotionStand):
    def launch(self, label, args, **env):
        if label.startswith("dispatch-"):
            if not hasattr(self, "database_proxy"):
                self.database_proxy = AdmissionProxy(self.stack, self.dbaddr)
            env["DATABASE_ADDRESS"] = f"127.0.0.1:{self.database_proxy.port}"
        elif label == "control":
            self.dispatch_proxy = AdmissionProxy(self.stack, self.fdaddr)
            env.update(DATABASE_ADDRESS=f"127.0.0.1:{self.database_proxy.port}",
                       DISPATCH_ADDRESS=f"127.0.0.1:{self.dispatch_proxy.port}")
        proc = super().launch(label, args, **env)
        if label == "simulator-D":
            self.d_sim = proc
        return proc

    def restart_without_a(self, hidden=False):
        self.prepare(crossing=False)
        self.trip(LEAD, "A", "C")
        self.state("A", LEAD, lambda s: s["orderUpdateId"] == 1 and s["driving"] and
                   s["lastNodeId"] == "B" and s["mobileRobotPosition"]["x"] > 1.4, "A physically on B-C")
        self.frozen_sim(True)
        self.stack.callback(self.frozen_sim, False)
        if hidden:
            self.dispatch_proxy.hide_lead_checkpoint.set()
        old = self.inventory().instance_id
        stop(self.fd, abrupt=True)
        self.dispatch()
        view = self.unresolved_a()
        assert old and view.instance_id != old
        assert all(m.mission_id != TRIP for m in view.missions)
        wait(lambda: (d := self.status("D")) and d.HasField("observed_order") and d.observed_order.idle
             and not d.mission_id and d.last_node_id == "E", "fresh idle D at E", 25)
        wait(lambda: self.assessment()["instanceId"] == view.instance_id,
             "control installed the new dispatch inventory before D is assigned", 25)
        assert self.orders("D", TRIP) == []
        return view.instance_id

    def assessment(self):
        code, result = self.http("GET", f"/api/fms/routes/recovery-assessment?missionId={TRIP}&robotId=D")
        assert code == 200, (code, result)
        return result

    def stay_at_e(self, *, rpc_counts=None):
        # Span at least one complete assignment safety period, including missed wakeups.
        deadline = time.monotonic() + 6.5
        while time.monotonic() < deadline:
            for raw in self.orders("D", TRIP):
                nodes = [n["nodeId"] for n in json.loads(raw)["nodes"] if n["released"]]
                assert nodes == ["E"], "unexpected motion grant while D must wait"
            if rpc_counts is not None:
                assert self.dispatch_proxy.issued == rpc_counts, "control sent after its admission expired"
            time.sleep(.1)
        self.broker_barrier()
        for raw in self.orders("D", TRIP):
            assert [n["nodeId"] for n in json.loads(raw)["nodes"] if n["released"]] == ["E"]
        if rpc_counts is not None:
            assert self.dispatch_proxy.issued == rpc_counts

    def disconnect_d(self):
        # The simulator uses a generic MQTT Last Will. Freeze its state publisher
        # and deliver the VDA connection loss explicitly on D's private topic.
        packet = next(obj for raw in reversed(self.wire.raw("connection"))
                      if (obj := json.loads(raw))["serialNumber"] == "amr102")
        os.killpg(self.d_sim.pid, signal.SIGSTOP)
        self.stack.callback(os.killpg, self.d_sim.pid, signal.SIGCONT)
        packet["connectionState"] = "CONNECTION_BROKEN"
        self.wire.client.publish("amr/v3/ktc/amr102/connection", json.dumps(packet), qos=1).wait_for_publish(2)
        wait(lambda: (d := self.status("D")) and d.connection == fms.CONNECTION_STATE_CONNECTION_BROKEN,
             "dispatch consumed D's connection loss", 8)


@pytest.mark.parametrize("case", ["disjoint", "crossing", "unknown"])
def test_new_assignment_after_dispatch_restart(case, tmp_path):
    with ExitStack() as stack:
        s = AssignmentStand(stack, tmp_path)
        run = s.restart_without_a(hidden=case == "unknown")
        s.trip(TRIP, "D", "C" if case == "crossing" else "F")

        if case != "disjoint":
            if case == "unknown":
                wait(lambda: s.dispatch_proxy.count("unknown A route") >= 2, "control retries A route proof", 25)
                wait(lambda: (v := s.assessment())["instanceId"] == run and not v["granted"] and
                     v["reason"] == "local command authority incomplete", "control lacks A's grant", 25)
            else:
                wait(lambda: (v := s.assessment())["instanceId"] == run and not v["granted"] and
                     v["reason"] == "requested route is occupied", "C in A's protected base", 25)
            s.unresolved_a()
            s.stay_at_e()
            robot = s.status("D")
            assert robot.last_node_id == "E" and not robot.driving
            if case == "unknown":
                s.dispatch_proxy.hide_lead_checkpoint.clear()
                s.state("D", TRIP, lambda r: r["orderUpdateId"] == 1 and r["driving"],
                        "verified A route allows the waiting D to depart")
                s.unresolved_a()
            return

        s.state("D", TRIP, lambda d: d["orderUpdateId"] == 1 and d["driving"] and d["lastNodeId"] == "E",
                "D drives on the new grant while A remains unresolved")
        calls = list(s.dispatch_proxy.issued)
        names = [name for name, _, _ in calls]
        assert "AddMissionDuringRecovery" in names and "ExtendMissionDuringRecovery" in names, names
        assert "AddMission" not in names and "ExtendMission" not in names
        for _, scope, _ in calls:
            assert scope.instance_id == run and scope.revision > 0 and scope.robot_id == "D"
            assert scope.HasField("add_attempt") and scope.add_attempt == 0

        # Lose dispatch once again after D accepts the new grant. The physical order and
        # journal must still contain exactly the original two issued versions.
        stop(s.fd, abrupt=True)
        s.dispatch()
        assert s.unresolved_a().instance_id != run
        s.arrived("D", TRIP, "F")
        wait(lambda: s.sealed(TRIP), "control seals D while A remains unresolved", 35)
        s.unresolved_a()
        s.broker_barrier()
        s.check_grants("A", LEAD, ["B", "C"])
        s.check_grants("D", TRIP, ["E", "M", "F"])
        rows = s.storage.ListControlCommands(db.ListControlCommandsRequest(
            mission_id=TRIP, include_resolved=True), timeout=3).commands
        durable = {row.command.kind: row.command.request_proto for row in rows}
        for name, _, cmd in calls:
            assert cmd == durable[ADD if name.startswith("Add") else EXTEND]


@pytest.mark.parametrize("kind", [ADD, EXTEND], ids=["add", "extend"])
def test_control_rechecks_after_journal(kind, tmp_path):
    with ExitStack() as stack:
        s = AssignmentStand(stack, tmp_path)
        s.restart_without_a()
        db_proxy, proxy = s.database_proxy, s.dispatch_proxy
        db_proxy.barrier = "PutControlCommand", kind
        s.trip(TRIP, "D", "F")
        try:
            assert db_proxy.persisted.wait(35), "journal command for D did not reach PostgreSQL"
            observed = list(proxy.issued)
            forbidden = ("AddMission", "AddMissionDuringRecovery") if kind == ADD else (
                "ExtendMission", "ExtendMissionDuringRecovery")
            assert all(name not in forbidden for name, _, _ in observed), "RPC overtook durable journal"
            proxy.block_inventory.set()
            wait(lambda: proxy.count("denied inventory") > 0 and s.assessment()["instanceId"] == "",
                 "control has discarded its recovery view", 10)
            db_proxy.release.set()
            assert db_proxy.acknowledged.wait(18), "journal reply never reached a live caller"
            s.stay_at_e(rpc_counts=observed)
            d = s.status("D")
            assert d.last_node_id == "E" and not d.driving
        finally:
            db_proxy.release.set()


@pytest.mark.parametrize("update", [0, 1], ids=["initial", "extend"])
def test_dispatch_rechecks_after_execution_write(update, tmp_path):
    with ExitStack() as stack:
        s = AssignmentStand(stack, tmp_path)
        s.restart_without_a()
        barrier = s.database_proxy
        barrier.barrier = "SaveMissionExecution", update
        s.trip(TRIP, "D", "F")
        try:
            assert barrier.persisted.wait(35), "D's execution record was not committed"
            assert all(json.loads(r)["orderUpdateId"] < update for r in s.orders("D", TRIP))
            s.disconnect_d()
            barrier.release.set()
            assert barrier.acknowledged.wait(18), "execution reply never reached a live caller"
            s.stay_at_e()
            assert all(json.loads(r)["orderUpdateId"] < update for r in s.orders("D", TRIP))
        finally:
            barrier.release.set()


def test_replay_unpublished_extension_after_admission_returns(tmp_path):
    with ExitStack() as stack:
        s = AssignmentStand(stack, tmp_path)
        s.restart_without_a()
        barrier = s.database_proxy
        barrier.barrier = "SaveMissionExecution", 1
        s.trip(TRIP, "D", "F")
        try:
            assert barrier.persisted.wait(35), "D's extension did not reach the dispatch journal"
            row, = s.sql.execute("SELECT execution FROM dispatch_execution_orders "
                                 "WHERE mission_id=%s AND update_id=1", (TRIP,)).fetchall()
            original = db.MissionExecution.FromString(bytes(row[0])).order.sent_order_json.encode()
            # Force the same fresh-proof loss as expiry while the original 3 s write
            # RPC is still live. D stays online at E with confirmed grant r0.
            barrier.block_compact_snapshot.set()

            def revoked():
                try:
                    s.inventory()
                except grpc.RpcError as error:
                    if error.code() == grpc.StatusCode.UNAVAILABLE:
                        return barrier.count("compact read failed") > 0
                    raise
                return False

            wait(revoked, "dispatch revoked admission before the execution acknowledgement", 6)
            barrier.release.set()
            assert barrier.acknowledged.wait(10), "missing acknowledgement to the first writer"
            wait(lambda: any(s.dispatch_proxy.count("ExtendMissionDuringRecovery " + status) for status in
                             ("ABORTED", "UNAVAILABLE", "DEADLINE_EXCEEDED")),
                 "dispatch has returned from the unissued Extend", 10)
            assert all(json.loads(r)["orderUpdateId"] == 0 for r in s.orders("D", TRIP))
            d = s.status("D")
            assert d.HasField("observed_order") and d.observed_order.update_id == 0 and not d.driving
        finally:
            barrier.release.set()
            barrier.block_compact_snapshot.clear()

        s.arrived("D", TRIP, "F")
        wait(lambda: s.sealed(TRIP), "D final after retry of the saved update", 35)
        s.unresolved_a()
        s.broker_barrier()
        s.check_grants("D", TRIP, ["E", "M", "F"])
        assert s.orders("D", TRIP)[1] == original
        sent = [cmd for name, _, cmd in s.dispatch_proxy.issued if name == "ExtendMissionDuringRecovery"]
        assert len(sent) >= 2 and all(cmd == sent[0] for cmd in sent)
