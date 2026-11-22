"""2.2в.3. Real dispatch, database, PostgreSQL, simulator and private MQTT broker on WSL."""
import json
import os
import shutil
import signal
import socket
import subprocess
import threading
import time
import uuid
from contextlib import ExitStack

import grpc
import paho.mqtt.client as mqtt
import pytest

import fleet_database_pb2 as db
import fleet_database_pb2_grpc as db_rpc
import fleet_dispatch_pb2 as dp
import fleet_dispatch_pb2_grpc as dp_rpc
import fms_types_pb2 as fms
from dispatch_client import move_mission, node
from test_dispatch_startup import ROOT, first

pytestmark = pytest.mark.skipif(os.getenv("MOVE_RESTART_STAND") != "1",
                                reason="isolated WSL acceptance stand")
RID = "recovery-test"
BASE = "amr/v3/ktc/amr101/"


def wait(read, description, seconds=15):
    end = time.monotonic() + seconds
    last = None
    while time.monotonic() < end:
        try:
            last = read()
            if last:
                return last
        except grpc.RpcError as error:
            if error.code() not in (grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.DEADLINE_EXCEEDED):
                raise
            last = error.details()
        time.sleep(.08)
    pytest.fail(f"timeout waiting for {description}: {last!r}")


def stop(proc, abrupt=False):
    if proc.poll() is not None:
        return
    os.killpg(proc.pid, signal.SIGKILL if abrupt else signal.SIGTERM)
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait(5)


class Wire:
    def __init__(self, port):
        self.events = []
        self.lock = threading.Lock()
        self.sub = threading.Event()
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_message = self.message
        self.client.on_subscribe = lambda c, u, m, codes, p: (
            self.sub.set() if codes and all(not x.is_failure for x in codes) else None)
        self.client.connect("127.0.0.1", port)
        self.client.loop_start()
        try:
            self.client.subscribe(BASE + "#", qos=1)
            assert self.sub.wait(5), "broker did not acknowledge test subscription"
        except BaseException:
            self.close()
            raise

    def close(self):
        self.client.disconnect()
        self.client.loop_stop()

    def message(self, _, __, msg):
        with self.lock:
            self.events.append((msg.topic.rsplit("/", 1)[1], bytes(msg.payload)))

    def mark(self):
        with self.lock:
            return len(self.events)

    def raw(self, kind, since=0):
        with self.lock:
            return [v for k, v in self.events[since:] if k == kind]

    def state(self, since, predicate, description, timeout=45):
        def observed():
            for raw in reversed(self.raw("state", since)):
                value = json.loads(raw)
                if predicate(value):
                    return value
        return wait(observed, description, timeout)


@pytest.mark.parametrize("case", ["moving", "base", "extend", "arrived", "final_write"])
def test_move_restart(case, tmp_path):
    import psycopg
    import yaml

    build = ROOT / "cmake-build-debug"
    for tool in ("mosquitto", "proot", "stdbuf"):
        assert shutil.which(tool), f"missing program: {tool}"
    config_file = ROOT / "robot_simulation/config/config.yaml"
    hardcoded = "/home/ubuntu/fleet-manager/robot_simulation/config/config.yaml"
    dbname = "fms_move_" + uuid.uuid4().hex[:16]
    url = f"postgresql://fms:fms@127.0.0.1:5432/{dbname}"
    print(f"case={case} database={dbname} logs={tmp_path}", flush=True)
    # The test creates this DB itself. An existing DB would make createdb fail.
    subprocess.run(["sudo", "-n", "-u", "postgres", "createdb", "-O", "fms", dbname], check=True)
    with ExitStack() as stack:
        def launch(name, args, **env):
            log = stack.enter_context((tmp_path / (name + ".log")).open("wb"))
            proc = subprocess.Popen(args, cwd=ROOT, start_new_session=True,
                                    env={**os.environ, **env}, stdout=log, stderr=subprocess.STDOUT)
            stack.callback(stop, proc)
            return proc

        # Reserve distinct ports until all four have been selected.
        with ExitStack() as sockets:
            ports = []
            for _ in range(4):
                s = sockets.enter_context(socket.socket())
                s.bind(("127.0.0.1", 0))
                ports.append(s.getsockname()[1])
        port, dbport, httpport, fdport = ports
        dbaddr, fdaddr = f"127.0.0.1:{dbport}", f"127.0.0.1:{fdport}"

        conf = tmp_path / "mosquitto.conf"
        conf.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
        broker = launch("mqtt", ["mosquitto", "-c", conf])

        def broker_ready():
            assert broker.poll() is None, "private broker exited; see mqtt.log"
            try:
                with socket.create_connection(("127.0.0.1", port), .3):
                    return True
            except OSError:
                return False
        wait(broker_ready, "private MQTT listener")
        wire = Wire(port)
        stack.callback(wire.close)

        cfg = yaml.safe_load(config_file.read_text())
        cfg["mqtt"] = dict(host="127.0.0.1", port=port, client_id="sim-recovery",
                           username="", password="")
        cfg["robots"] = [dict(serial_number="101", manufacturer="ktc", interface_name="amr")]
        cfg["initial_state"].update(map_id="map_1", x=0.0, y=0.0, theta=0.0)
        cfg["motion"].update(max_speed=1.0)
        cfg["behavior_tree"]["xml_path"] = str(ROOT / "robot_simulation/trees/robot_tree.xml")
        simconfig = tmp_path / "simulator.yaml"
        simconfig.write_text(yaml.safe_dump(cfg))
        sim = launch("simulator", ["proot", "-b", f"{simconfig}:{hardcoded}",
                                   build / "robot_simulation/robot_simulation"], PROOT_NO_SECCOMP="1")
        database = launch("database", [build / "fleet_database/fleet_database"],
                          DATABASE_URL=url, GRPC_ADDRESS=dbaddr, HTTP_ADDRESS=f"127.0.0.1:{httpport}",
                          MIGRATIONS_DIR=str(ROOT / "fleet_database/migrations"))
        dbc = stack.enter_context(grpc.insecure_channel(dbaddr))
        store = db_rpc.DispatchStorageStub(dbc)
        def inventory():
            return store.LoadDispatchSnapshot(db.LoadDispatchSnapshotRequest(), timeout=2).snapshot
        wait(lambda: inventory().ByteSize() == 0, "empty migrated database", 30)
        sql = stack.enter_context(psycopg.connect(url, autocommit=True))
        mid = "restart-" + case
        oid = mid + "-s1"

        def head():
            row = sql.execute("SELECT execution FROM dispatch_execution_heads WHERE mission_id=%s",
                              (mid,)).fetchone()
            return db.MissionExecution.FromString(bytes(row[0])) if row and row[0] else None

        def dispatch(epoch):
            label = f"dispatch-{epoch}"
            proc = launch(label, ["stdbuf", "-oL", build / "fleet_dispatch/fleet_dispatch"],
                          DISPATCH_LISTEN_ADDRESS=fdaddr, DATABASE_ADDRESS=dbaddr, MQTT_PORT=str(port),
                          MQTT_HOST="127.0.0.1", MQTT_USER="", MQTT_PASSWORD="")
            channel = stack.enter_context(grpc.insecure_channel(fdaddr))
            api = dp_rpc.FleetDispatchStub(channel)
            def ready():
                assert proc.poll() is None, f"{label} exited; see {label}.log"
                return first(api.StreamRobotStatus, dp.StreamRobotStatusRequest()).HasField("snapshot")
            wait(ready, f"{label} loaded inventory")
            return proc, api

        def owner(api):
            robots = first(api.StreamRobotStatus, dp.StreamRobotStatusRequest()).snapshot.robots
            return next((r for r in robots if r.robot_id == RID), None)

        def finished(s):
            return (s["orderId"] == oid and not s["driving"] and s["lastNodeId"] == "C"
                    and not s["nodeStates"] and not s["edgeStates"])

        fd, api = dispatch(1)
        wire.state(0, lambda s: not s["driving"], "simulator startup")
        db_rpc.RobotRegistryStub(dbc).CreateRobot(db.CreateRobotRequest(
            spec=db.RobotSpec(robot_id=RID, manufacturer="ktc", serial_number="amr101")), timeout=5)
        api.AddRobot(dp.AddRobotRequest(robot_id=RID, manufacturer="ktc", serial_number="amr101"), timeout=5)
        wait(lambda: (r := owner(api)) and r.connection == fms.CONNECTION_STATE_ONLINE, "online robot")
        api.InitializePosition(dp.InitializePositionRequest(robot_id=RID, last_node_id="A",
            pose=fms.Pose(map_id="map_1", x=0, y=0, theta=0)), timeout=5)
        wait(lambda: owner(api).last_node_id == "A", "dispatch sees initial node A")
        initial = wire.mark()
        limited = case in ("base", "extend")
        mission = move_mission(mid, RID, [node("A", 0, map_id="map_1"), node("B", 3, map_id="map_1"),
                                          node("C", 15, map_id="map_1")],
                               released_nodes=2 if limited else 3)
        api.AddMission(dp.AddMissionRequest(mission=mission), timeout=8)
        if limited:
            wire.state(initial, lambda s: s["orderId"] == oid and s["lastNodeId"] == "B"
                       and not s["driving"] and s["nodeStates"] and
                       all(not n["released"] for n in s["nodeStates"]), "end of released base")
        if case == "extend":
            api.ExtendMission(dp.ExtendMissionRequest(mission_id=mid, move_node_id="s1",
                              revision=1, released_nodes=3), timeout=6)
        revision = int(case == "extend")
        if case != "base":
            wire.state(initial, lambda s: s["orderId"] == oid and s["orderUpdateId"] == revision
                       and s["driving"], "robot moving on expected grant")
        old = head()
        assert old and old.status.state == fms.MISSION_STATE_RUNNING
        assert old.order.order_update_id == revision
        expected_orders = [bytes(row[0]) for row in sql.execute(
            "SELECT execution FROM dispatch_execution_orders WHERE mission_id=%s ORDER BY update_id",
            (mid,)).fetchall()]
        payloads = [db.MissionExecution.FromString(e).order.sent_order_json.encode() for e in expected_orders]
        assert len(payloads) == revision + 1
        wait(lambda: len(wire.raw("order", initial)) >= len(payloads), "original grants on MQTT")
        assert wire.raw("order", initial) == payloads

        stop(fd, abrupt=True)
        if case == "arrived":
            wire.state(initial, finished, "arrival while dispatch is down")
            assert head().status.state == fms.MISSION_STATE_RUNNING
        restart = wire.mark()
        fd, api = dispatch(2)
        def startup_reply(mark):
            def answered(s):
                tokens = {a["actionId"] for raw in wire.raw("instantActions", mark)
                          for a in json.loads(raw)["actions"] if a["actionType"] == "stateRequest"}
                return any(a["actionId"] in tokens and a["actionStatus"] == "FINISHED"
                           for a in s["instantActionStates"])
            return wire.state(mark, answered, "reply to this dispatch process")
        fresh = startup_reply(restart)
        assert fresh["orderId"] == oid
        wait(lambda: "result=Attach " in (tmp_path / "dispatch-2.log").read_text(),
             "dispatch attached existing move")
        if case in ("moving", "extend", "final_write"):
            assert fresh["driving"], "stand missed the moving recovery window"
        if case == "arrived":
            assert finished(fresh)

        if case == "base":
            assert fresh["lastNodeId"] == "B" and not fresh["driving"]
            assert [(n["nodeId"], n["released"]) for n in fresh["nodeStates"]] == [("C", False)]
            # The external Extend gate remains closed in 2.2v.2.
            with pytest.raises(grpc.RpcError) as err:
                api.ExtendMission(dp.ExtendMissionRequest(mission_id=mid, move_node_id="s1",
                                  revision=1, released_nodes=3), timeout=3)
            assert err.value.code() == grpc.StatusCode.UNAVAILABLE
            time.sleep(2)
            assert owner(api).mission_id == mid
            assert head() == old
            assert all(not s["driving"] and s["lastNodeId"] == "B"
                       for s in map(json.loads, wire.raw("state", restart)) if s["orderId"] == oid)
        else:
            if case == "final_write":
                # Freeze only writes to the execution head. Wait until PostgreSQL itself
                # confirms that the final writer is waiting behind our lock.
                with psycopg.connect(url) as holder:
                    holder.execute("SET LOCAL idle_in_transaction_session_timeout='75s'")
                    holder.execute("SET LOCAL lock_timeout='4s'")
                    holder.execute("LOCK TABLE dispatch_execution_heads IN SHARE MODE")
                    wire.state(restart, finished, "arrival before the final write")
                    wait(lambda: sql.execute(
                        "SELECT EXISTS (SELECT FROM pg_locks WHERE NOT granted AND "
                        "database=(SELECT oid FROM pg_database WHERE datname=current_database()) AND "
                        "relation='dispatch_execution_heads'::regclass AND %s=ANY(pg_blocking_pids(pid)))",
                        (holder.info.backend_pid,)).fetchone()[0], "blocked final checkpoint", 6)
                    assert owner(api).mission_id == mid and head() == old
                    stop(fd, abrupt=True)
                # fleet_database survives the crash and finishes the in-flight transaction.
                wait(lambda: head().status.state == fms.MISSION_STATE_COMPLETED, "in-flight DB commit")
                durable = head()
                third_start = wire.mark()
                fd, api = dispatch(3)
                assert finished(startup_reply(third_start))
            else:
                wire.state(initial, finished, "physical arrival")
            wait(lambda: head().status.state == fms.MISSION_STATE_COMPLETED, "completed DB checkpoint")
            final = head()
            assert final.checkpoint_version == old.checkpoint_version + 1
            assert final.mission == old.mission and not final.HasField("order")
            step, = final.status.steps
            assert step.state == fms.STEP_STATE_COMPLETED and step.attempts == 1
            assert (step.move.last_node_id, step.move.last_node_index) == ("C", 2)
            assert step.move.confirmed_revision == revision
            assert all(e.mission.mission_id != mid for e in inventory().active_missions)
            assert all(e.mission_id != mid for e in inventory().unresolved)
            wait(lambda: (r := owner(api)) and not r.mission_id, "robot released")
            if case == "final_write":
                assert final == durable
        assert fd.poll() is None and database.poll() is None and sim.poll() is None
        # Stop the publisher before taking the final wire inventory. Round-trip a marker
        # through this same subscription to drain preceding messages from the broker.
        stop(fd)
        wire.client.publish(BASE + "barrier", b"end", qos=1).wait_for_publish(3)
        wait(lambda: wire.raw("barrier") == [b"end"], "MQTT observation barrier")
        assert wire.raw("order", initial) == payloads
        assert all(action["actionType"] == "stateRequest"
                   for raw in wire.raw("instantActions", restart)
                   for action in json.loads(raw)["actions"])
    subprocess.run(["sudo", "-n", "-u", "postgres", "dropdb", dbname], check=True)
