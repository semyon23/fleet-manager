"""2.3.2. Private WSL acceptance stand with real control, dispatch, DB, MQTT and simulator."""
import json
import os
import shutil
import signal
import socket
import subprocess
import threading
import time
import uuid
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

import grpc
import pytest

import fleet_database_pb2 as db
import fleet_database_pb2_grpc as db_rpc
import fleet_dispatch_pb2 as dp
import fleet_dispatch_pb2_grpc as dp_rpc
import fms_types_pb2 as fms
from test_dispatch_startup import ROOT, first
from test_move_restart_stand import Wire, stop, wait

pytestmark = pytest.mark.skipif(os.getenv("CONTROL_CHECKPOINT_STAND") != "1",
                                reason="private WSL integration stand")
RID, MID = "recovery-test", "control-trip"
OID = MID + "-s0"


class Proxy(grpc.GenericRpcHandler):
    """Faults affect only control's connections. Other clients talk directly to the services."""

    def __init__(self, stack, upstream):
        self.channel = stack.enter_context(grpc.insecure_channel(upstream))
        self.pool = stack.enter_context(ThreadPoolExecutor(max_workers=16))
        self.server = grpc.server(self.pool)
        self.server.add_generic_rpc_handlers((self,))
        self.port = self.server.add_insecure_port("127.0.0.1:0")
        assert self.port
        self.lock = threading.Lock()
        self.streams = set()
        self.counts = Counter()
        self.block_missions = False
        self.omit_terminal = False
        self.lookup_error = None
        self.robot_mode = "live"
        self.block_save = False
        self.block_close = False
        self.lose_save_ack = False
        self.lose_close_ack = False
        self.last_save = None
        self.closing = None
        self.server.start()
        stack.callback(lambda: self.server.stop(0).wait(5))

    def hit(self, name):
        with self.lock:
            self.counts[name] += 1

    def count(self, name):
        with self.lock:
            return self.counts[name]

    def disconnect_missions(self):
        with self.lock:
            self.block_missions = True
            calls = list(self.streams)
        for call in calls:
            call.cancel()

    def service(self, details):
        method = details.method
        name = method.rsplit("/", 1)[-1]

        def unary(raw, ctx):
            self.hit(name)
            if name == "GetExecutionCheckpoint":
                assert fms.GetExecutionCheckpointRequest.FromString(raw).mission_id == MID
                if self.lookup_error is not None:
                    self.hit(self.lookup_error.name)
                    ctx.abort(self.lookup_error, "injected lookup failure")
            final_save = name == "SaveMission" and db.SaveMissionRequest.FromString(raw).mission.dispatch_final_version
            if final_save:
                self.last_save = bytes(raw)
                if self.block_save:
                    self.hit("save blocked")
                    ctx.abort(grpc.StatusCode.UNAVAILABLE, "injected before final commit")
            if name == "CloseMissionCommands":
                self.closing = db.CloseMissionCommandsRequest.FromString(raw)
                if self.block_close:
                    self.hit("close blocked")
                    ctx.abort(grpc.StatusCode.UNAVAILABLE, "injected before journal close")
            try:
                reply = self.channel.unary_unary(method)(raw, timeout=3)
            except grpc.RpcError as error:
                ctx.abort(error.code(), error.details())
            if name == "GetExecutionCheckpoint":
                kind = fms.GetExecutionCheckpointResponse.FromString(reply).kind
                self.hit(f"lookup {kind}")
            if name == "CloseMissionCommands" and self.lose_close_ack:
                self.lose_close_ack = False
                self.hit("lost close ack")
                ctx.abort(grpc.StatusCode.UNAVAILABLE, "injected after journal close")
            if final_save and self.lose_save_ack:
                self.lose_save_ack = False
                self.hit("lost final ack")
                ctx.abort(grpc.StatusCode.UNAVAILABLE, "injected after final commit")
            return reply

        def stream(raw, ctx):
            if name == "StreamMissionStatus" and self.block_missions:
                ctx.abort(grpc.StatusCode.UNAVAILABLE, "mission stream disconnected")
            call = self.channel.unary_stream(method)(raw)
            ctx.add_callback(call.cancel)
            try:
                if name == "StreamMissionStatus":
                    with self.lock:
                        self.streams.add(call)
                        blocked = self.block_missions
                    if blocked:
                        call.cancel()
                for reply in call:
                    if name == "StreamMissionStatus" and self.omit_terminal:
                        msg = dp.MissionStatusUpdate.FromString(reply)
                        if msg.HasField("status") and msg.status.mission_id == MID and msg.status.state == fms.MISSION_STATE_COMPLETED:
                            missing = dp.MissionStatusUpdate()
                            missing.snapshot.SetInParent()
                            self.hit("own mission omitted")
                            yield missing.SerializeToString()
                            continue
                    if name == "StreamRobotStatus":
                        msg = dp.RobotStatusUpdate.FromString(reply)
                        robots = msg.snapshot.robots if msg.HasField("snapshot") else (
                            [msg.status] if msg.HasField("status") else [])
                        mode = self.robot_mode
                        for robot in robots:
                            if robot.robot_id != RID:
                                continue
                            if mode == "cached":
                                robot.ClearField("observed_order")
                            elif mode == "braking":
                                robot.driving = True
                            if robot.last_node_id == "C":
                                self.hit("position C " + mode)
                        reply = msg.SerializeToString()
                    yield reply
            except grpc.RpcError as error:
                if ctx.is_active():
                    ctx.abort(error.code(), error.details())
            finally:
                call.cancel()
                with self.lock:
                    self.streams.discard(call)

        if name in {"StreamRobotStatus", "StreamMissionStatus", "Watch"}:
            return grpc.unary_stream_rpc_method_handler(stream)
        return grpc.unary_unary_rpc_method_handler(unary)


class Stand:
    def __init__(self, stack, tmp):
        import psycopg
        import yaml

        self.stack, self.tmp = stack, tmp
        self.build = ROOT / "cmake-build-debug"
        for tool in ("mosquitto", "proot", "stdbuf"):
            assert shutil.which(tool), f"missing {tool}"
        name = "fms_controlcp_" + uuid.uuid4().hex[:16]
        self.url = f"postgresql://fms:fms@127.0.0.1:5432/{name}"
        print(f"database={name} logs={tmp}", flush=True)
        pg = ["sudo", "-n", "-u", "postgres"]
        subprocess.run([*pg, "createdb", "-O", "fms", name], check=True)
        stack.callback(subprocess.run, [*pg, "dropdb", name], check=True)
        with ExitStack() as reserved:
            ports = []
            for _ in range(5):
                sock = reserved.enter_context(socket.socket())
                sock.bind(("127.0.0.1", 0))
                ports.append(sock.getsockname()[1])
        mqtt_port, dbport, dbhttp, fdport, ctlport = ports
        self.mqtt_port = mqtt_port
        self.dbaddr = f"127.0.0.1:{dbport}"
        self.fdaddr = f"127.0.0.1:{fdport}"
        self.ctladdr = f"127.0.0.1:{ctlport}"
        conf = tmp / "mosquitto.conf"
        conf.write_text(f"listener {mqtt_port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
        broker = self.launch("broker", ["mosquitto", "-c", conf])

        def broker_ready():
            assert broker.poll() is None
            try:
                with socket.create_connection(("127.0.0.1", mqtt_port), .3):
                    return True
            except OSError:
                return False
        wait(broker_ready, "private MQTT broker")
        self.wire = Wire(mqtt_port)
        stack.callback(self.wire.close)
        cfg_path = ROOT / "robot_simulation/config/config.yaml"
        cfg = yaml.safe_load(cfg_path.read_text())
        cfg["mqtt"] = dict(host="127.0.0.1", port=mqtt_port, client_id="private-controlcp",
                           username="", password="")
        cfg["robots"] = [dict(serial_number="101", manufacturer="ktc", interface_name="amr")]
        cfg["initial_state"].update(map_id="map_1", x=1., y=2.5, theta=0.)
        cfg["motion"]["max_speed"] = 1.
        # Pose changes alone do not trigger state. Sample it during the B-C trip.
        cfg["publisher"]["max_state_interval_ms"] = 500
        cfg["behavior_tree"]["xml_path"] = str(ROOT / "robot_simulation/trees/robot_tree.xml")
        config = tmp / "simulator.yaml"
        config.write_text(yaml.safe_dump(cfg))
        target = "/home/ubuntu/fleet-manager/robot_simulation/config/config.yaml"
        self.sim = self.launch("simulator", ["proot", "-b", f"{config}:{target}",
                               self.build / "robot_simulation/robot_simulation"], PROOT_NO_SECCOMP="1")
        self.launch("database", [self.build / "fleet_database/fleet_database"],
                    DATABASE_URL=self.url, GRPC_ADDRESS=self.dbaddr, HTTP_ADDRESS=f"127.0.0.1:{dbhttp}",
                    MIGRATIONS_DIR=str(ROOT / "fleet_database/migrations"))
        chan = stack.enter_context(grpc.insecure_channel(self.dbaddr))
        self.storage = db_rpc.ControlStorageStub(chan)
        store = db_rpc.DispatchStorageStub(chan)
        empty = store.LoadDispatchSnapshot(db.LoadDispatchSnapshotRequest(), wait_for_ready=True, timeout=30)
        assert not empty.snapshot.robots
        self.sql = stack.enter_context(psycopg.connect(self.url, autocommit=True))
        self.dbproxy = Proxy(stack, self.dbaddr)
        self.fdproxy = Proxy(stack, self.fdaddr)
        self.api = dp_rpc.FleetDispatchStub(stack.enter_context(grpc.insecure_channel(self.fdaddr)))
        self.fd = self.ctl = None
        self.epoch = Counter()
        self.dispatch()
        self.control()

    def launch(self, label, args, **env):
        out = self.stack.enter_context((self.tmp / (label + ".log")).open("wb"))
        proc = subprocess.Popen(args, cwd=ROOT, start_new_session=True, env={**os.environ, **env},
                                stdout=out, stderr=subprocess.STDOUT)
        self.stack.callback(stop, proc)
        return proc

    def dispatch(self):
        self.epoch["dispatch"] += 1
        self.fd = self.launch("dispatch-" + str(self.epoch["dispatch"]),
                              ["stdbuf", "-oL", self.build / "fleet_dispatch/fleet_dispatch"],
                              DISPATCH_LISTEN_ADDRESS=self.fdaddr, DATABASE_ADDRESS=self.dbaddr,
                              MQTT_PORT=str(self.mqtt_port), MQTT_HOST="127.0.0.1", MQTT_USER="", MQTT_PASSWORD="")
        wait(lambda: first(self.api.StreamRobotStatus, dp.StreamRobotStatusRequest()).HasField("snapshot"),
             "dispatch inventory", 25)

    def control(self):
        self.epoch["control"] += 1
        self.ctl = self.launch("control-" + str(self.epoch["control"]),
                               ["stdbuf", "-oL", self.build / "fleet_control/fleet_control"],
                               DATABASE_ADDRESS=f"127.0.0.1:{self.dbproxy.port}",
                               DISPATCH_ADDRESS=f"127.0.0.1:{self.fdproxy.port}", HTTP_ADDRESS=self.ctladdr)
        wait(lambda: self.http("GET", "/api/health")[0] == 200, "control HTTP", 25)

    def http(self, method, path, body=None):
        req = Request("http://" + self.ctladdr + path, method=method,
                      data=None if body is None else json.dumps(body).encode(),
                      headers={"Content-Type": "application/json"})
        try:
            with urlopen(req, timeout=3) as reply:
                return reply.status, json.loads(reply.read() or "{}")
        except HTTPError as error:
            return error.code, json.loads(error.read() or "{}")
        except (URLError, TimeoutError, ConnectionError):
            return 0, None

    def robot(self):
        snapshot = first(self.api.StreamRobotStatus, dp.StreamRobotStatusRequest()).snapshot
        return next((r for r in snapshot.robots if r.robot_id == RID), None)

    def document(self):
        return self.storage.GetMission(db.GetMissionRequest(mission_id=MID), timeout=3).mission

    def commands(self):
        response = self.storage.ListControlCommands(
            db.ListControlCommandsRequest(mission_id=MID, include_resolved=True), timeout=3)
        return list(response.commands)

    def history(self, state):
        return self.sql.execute("SELECT count(*) FROM events WHERE mission_id=%s AND source='MISSION' "
                                "AND type='STATE' AND details->>'state'=%s", (MID, state)).fetchone()[0]

    def start_trip(self):
        layout = dict(id="map_1", name="Recovery", width=400, height=100,
                      meta={"resolution": .05, "origin": [0, 0, 0]},
                      waypoints=[{"id": "B", "u": 20, "v": 50}, {"id": "C", "u": 220, "v": 50}],
                      edges=[{"id": "BC", "from": "B", "to": "C"}, {"id": "CB", "from": "C", "to": "B"}],
                      stations=[], zones=[], assignedRobots=[])
        def create_map():
            code, body = self.http("POST", "/api/fms/maps", layout)
            if code == 503:
                return False
            assert code == 201, (code, body)
            return True
        wait(create_map, "map loaded")
        code, body = self.http("POST", "/api/fms/robots", dict(id=RID, manufacturer="ktc", serialNumber="amr101"))
        assert code == 201, (code, body)
        wait(lambda: (r := self.robot()) and r.connection == fms.CONNECTION_STATE_ONLINE, "online simulator")
        code, body = self.http("POST", f"/api/fms/robots/{RID}/position", dict(mapId="map_1", nodeId="B"))
        assert code == 202, (code, body)
        wait(lambda: (r := self.robot()) and r.last_node_id == "B" and r.HasField("observed_order"),
             "fresh initial B", 25)
        self.mark = self.wire.mark()
        code, body = self.http("POST", "/api/fms/missions", dict(id=MID, name="Trip", mapId="map_1",
                               steps=[{"nodeId": "C"}], requestedRobotId=RID))
        assert code == 201, (code, body)
        self.wire.state(self.mark, lambda s: s["orderId"] == OID and s["orderUpdateId"] == 1 and
                        s["driving"] and 2 < s["mobileRobotPosition"]["x"] < 8, "moving between B and C")
        wait(lambda: len(self.wire.raw("order", self.mark)) >= 2, "both MQTT orders")
        self.orders = self.wire.raw("order", self.mark)
        assert [json.loads(raw)["orderUpdateId"] for raw in self.orders] == [0, 1]

    def guard(self, closed=False, cancel=False):
        rows = self.commands()
        expected = [db.CONTROL_COMMAND_KIND_ADD_MISSION, db.CONTROL_COMMAND_KIND_EXTEND_MISSION]
        if cancel:
            expected.append(db.CONTROL_COMMAND_KIND_CANCEL_MISSION)
        assert [r.command.kind for r in rows] == expected
        assert all(r.resolved == closed for r in rows)
        base = json.loads(rows[-1].command.recovery_json)["protectedBase"]
        assert (base["robot"], base["nodes"], base["edges"], base["reached"]) == (RID, [0, 1], [0], 0)
        assert self.wire.raw("order", self.mark) == self.orders

    def stable_guard(self, cancel=False):
        # Exceeds the maximum recovery retry interval (4 s), including scheduling.
        until = time.monotonic() + 4.8
        while time.monotonic() < until:
            self.guard(cancel=cancel)
            time.sleep(.25)

    def frozen_sim(self, freeze):
        os.killpg(self.sim.pid, signal.SIGSTOP if freeze else signal.SIGCONT)

    def wait_final(self, state):
        def done():
            m = self.document()
            return m if m.state == state and m.dispatch_final_version else None
        return wait(done, "sealed " + state, 35)


@pytest.mark.parametrize("case", ["cold_commit", "dispatch_only", "cancel_between", "active_reconnect", "missing_from_snapshot"])
def test_control_recovery(case, tmp_path):
    with ExitStack() as stack:
        s = Stand(stack, tmp_path)
        s.start_trip()
        dp_fault, db_fault = s.fdproxy, s.dbproxy
        if case == "cancel_between":
            code, body = s.http("POST", f"/api/fms/missions/{MID}/cancel")
            assert code in (200, 202), (code, body)
            s.wait_final("CANCELED")
            actual = s.wire.state(s.mark, lambda x: x["orderId"] == OID and not x["driving"] and
                                  any(a.get("actionType") == "cancelOrder" and a["actionStatus"] == "FINISHED"
                                      for a in x.get("instantActionStates", [])), "stop on edge")
            assert actual["lastNodeId"] == "B" and 1.5 < actual["mobileRobotPosition"]["x"] < 10.5
            stop(s.ctl, abrupt=True)
            dp_fault.disconnect_missions()
            before = dp_fault.count("GetExecutionCheckpoint")
            s.control()
            wait(lambda: dp_fault.count("GetExecutionCheckpoint") > before, "cold cancel lookup")
            s.stable_guard(cancel=True)
            assert s.document().state == "CANCELED" and s.history("CANCELED") == 1
            assert db_fault.count("CloseMissionCommands") == 0
            actions = [a for raw in s.wire.raw("instantActions", s.mark)
                       for a in json.loads(raw)["actions"] if a["actionType"] == "cancelOrder"]
            assert len(actions) == 1
            return

        if case == "active_reconnect":
            s.frozen_sim(True)
            try:
                dp_fault.disconnect_missions()
                wait(lambda: dp_fault.count(f"lookup {fms.GetExecutionCheckpointResponse.ACTIVE}") > 0,
                     "durable active with mission stream closed")
                s.guard()
                dp_fault.block_missions = False
                wait(lambda: "route restored from dispatch" in (tmp_path / "control-1.log").read_text(),
                     "live active route restored", 30)
            finally:
                s.frozen_sim(False)
        elif case == "missing_from_snapshot":
            before = dp_fault.count("GetExecutionCheckpoint")
            dp_fault.omit_terminal = True
            wait(lambda: dp_fault.count("own mission omitted"), "fresh snapshot without own mission", 35)
            wait(lambda: dp_fault.count("GetExecutionCheckpoint") > before, "lookup for the missing mission")
        elif case == "dispatch_only":
            stop(s.fd, abrupt=True)
            dp_fault.disconnect_missions()
            dp_fault.lookup_error = grpc.StatusCode.NOT_FOUND
            wait(lambda: dp_fault.count("NOT_FOUND"), "addressed lookup without a mission stream")
            s.guard()
            dp_fault.lookup_error = grpc.StatusCode.UNAVAILABLE
            wait(lambda: dp_fault.count("UNAVAILABLE"), "checkpoint service unavailable")
            s.wire.state(s.mark, lambda x: x["orderId"] == OID and x["lastNodeId"] == "C" and
                         not x["driving"], "arrival while only dispatch is down")
            s.dispatch()
            assert s.document().state in {"ASSIGNED", "RUNNING"}
            s.guard()
            dp_fault.lookup_error = None
        else:
            stop(s.ctl, abrupt=True)
            dp_fault.disconnect_missions()
            dp_fault.robot_mode = "cached"
            db_fault.block_save = True
            db_fault.block_close = True
            s.wire.state(s.mark, lambda x: x["lastNodeId"] == "C" and not x["driving"], "missed arrival")
            s.control()
            wait(lambda: db_fault.count("save blocked"), "final write unavailable", 30)
            assert s.document().state in {"ASSIGNED", "RUNNING"}
            s.guard()
            db_fault.lose_save_ack = True
            db_fault.block_save = False
            s.wait_final("COMPLETED")
            wait(lambda: db_fault.count("lost final ack"), "lost reply after successful final commit")
            wait(lambda: dp_fault.count("position C cached"), "saved-looking pose C")
            s.stable_guard()
            assert db_fault.count("CloseMissionCommands") == 0
            dp_fault.robot_mode = "braking"
            wait(lambda: dp_fault.count("position C braking"), "robot still braking at C")
            s.stable_guard()
            assert db_fault.count("CloseMissionCommands") == 0
            dp_fault.robot_mode = "live"
            wait(lambda: db_fault.count("close blocked"), "journal close before second crash", 20)
            stop(s.ctl, abrupt=True)
            repeat = db.SaveMissionRequest.FromString(db_fault.last_save)
            s.storage.SaveMission(repeat, timeout=4)
            stale = db.SaveMissionRequest.FromString(db_fault.last_save)
            stale.mission.dispatch_final_version = 0
            with pytest.raises(grpc.RpcError) as err:
                s.storage.SaveMission(stale, timeout=4)
            assert err.value.code() == grpc.StatusCode.FAILED_PRECONDITION
            wrong = db.CloseMissionCommandsRequest()
            wrong.CopyFrom(db_fault.closing)
            del wrong.command_ids[-1]
            with pytest.raises(grpc.RpcError) as err:
                s.storage.CloseMissionCommands(wrong, timeout=4)
            assert err.value.code() == grpc.StatusCode.FAILED_PRECONDITION
            assert s.history("COMPLETED") == 1
            s.guard()
            # Real dispatch must withhold the persisted pose until its new stateRequest returns.
            s.frozen_sim(True)
            try:
                stop(s.fd, abrupt=True)
                s.dispatch()
                r = s.robot()
                assert r.last_node_id == "C" and not r.HasField("observed_order")
                db_fault.block_close = False
                db_fault.lose_close_ack = True
                before_close = db_fault.count("CloseMissionCommands")
                before = dp_fault.count("GetExecutionCheckpoint")
                s.control()
                wait(lambda: dp_fault.count("GetExecutionCheckpoint") > before, "lookup after second control crash")
                s.stable_guard()
            finally:
                s.frozen_sim(False)

        sealed = s.wait_final("COMPLETED")
        wait(lambda: (rows := s.commands()) and all(r.resolved for r in rows), "protected base handed off", 35)
        if case == "cold_commit":
            wait(lambda: db_fault.count("lost close ack") == 1 and
                 db_fault.count("CloseMissionCommands") >= before_close + 2, "idempotent close after lost ack", 20)
        s.wire.client.publish("amr/v3/ktc/amr101/barrier", b"end", qos=1).wait_for_publish(3)
        wait(lambda: s.wire.raw("barrier") == [b"end"], "MQTT observer caught up")
        s.guard(closed=True)
        r = s.robot()
        assert r.last_node_id == "C" and not r.driving and not r.mission_id
        assert r.HasField("observed_order") and r.observed_order.order_id == OID
        assert r.observed_order.idle and r.observed_order.node_sequence == 2
        head = s.api.GetExecutionCheckpoint(fms.GetExecutionCheckpointRequest(mission_id=MID), timeout=5)
        assert sealed.dispatch_final_version == head.checkpoint_version
        assert s.history("COMPLETED") == 1
        if case == "cold_commit":
            stop(s.ctl)
            check_inventory_pages(s.storage, s.commands()[0].command)


def check_inventory_pages(storage, original):
    # With closed missions and more than one page of open missions, startup
    # must see every open owner exactly once, without fetching route payloads.
    expected = []
    for i in range(130):
        mid = f"pending-{i:03}"
        attempt = i % 3
        c = db.ControlCommandRecord()
        c.CopyFrom(original)
        c.command_id, c.mission_id, c.attempt = mid + "-add", mid, attempt
        add = dp.AddMissionRequest.FromString(c.request_proto)
        add.mission.mission_id, add.attempt = mid, attempt
        c.request_proto = add.SerializeToString()
        snap = json.loads(c.recovery_json)
        snap["attempt"], snap["mission"]["id"] = attempt, mid
        snap["protectedBase"]["key"] = f"{mid}/{attempt}/s0"
        c.recovery_json = json.dumps(snap)
        storage.PutControlCommand(db.PutControlCommandRequest(command=c), timeout=3)
        expected.append((mid, attempt))
    page = storage.ListOpenControlMissions(db.ListOpenControlMissionsRequest(), timeout=3)
    assert len(page.missions) == 128 and page.next_cursor == expected[127][0]
    rest = storage.ListOpenControlMissions(db.ListOpenControlMissionsRequest(after_mission_id=page.next_cursor),
                                           timeout=3)
    assert not rest.next_cursor
    assert [(m.mission_id, m.last_attempt) for m in [*page.missions, *rest.missions]] == expected
