"""Restart the actual dispatch process with a private broker and a fixed DB snapshot."""
import json
import os
import queue
import socket
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager

import grpc
import paho.mqtt.client as mqtt
import pytest

from test_dispatch_startup import ROOT, Storage, db, db_rpc, dp, dp_rpc, first, fms, rejected

TOPIC = "amr/v3/K/101/"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@contextmanager
def process(args, log, env=None):
    with log.open("w") as out:
        proc = subprocess.Popen(args, env=env, stdout=out, stderr=subprocess.STDOUT)
        try:
            yield proc
        finally:
            proc.terminate()
            try:
                proc.wait(8)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                pytest.fail(f"process did not terminate: {log}")


def checkpoint():
    snapshot = db.DispatchSnapshot()
    r = snapshot.robots.add().spec
    r.robot_id, r.manufacturer, r.serial_number = "r", "K", "101"
    r.lifecycle = db.ROBOT_LIFECYCLE_ACTIVE
    e = snapshot.active_missions.add(checkpoint_version=1)
    e.mission.mission_id, e.mission.robot_id = "m", "r"
    move = e.mission.nodes.add(id="s").move
    move.nodes.add(node_id="A", map_id="map")
    e.status.mission_id, e.status.robot_id, e.status.current_node_id = "m", "r", "s"
    e.status.state = fms.MISSION_STATE_RUNNING
    s = e.status.steps.add(node_id="s", attempts=1, state=fms.STEP_STATE_RUNNING)
    s.move.total_nodes = s.move.released_nodes = 1
    s.move.last_node_id = "A"
    e.order.order_id, e.order.mission_node_id = "m-s", "s"
    e.order.sent_order_json = json.dumps(dict(orderId="m-s", orderUpdateId=0,
        manufacturer="K", serialNumber="101", edges=[], nodes=[dict(nodeId="A",
        sequenceId=0, released=True, actions=[], nodePosition=dict(x=0, y=0, mapId="map"))]))
    snapshot.last_grants.add().CopyFrom(e)
    return snapshot


def state(token, order, seq):
    return json.dumps(dict(headerId=seq, timestamp="2026-01-01T00:00:00.000Z",
        version="3.0.0", manufacturer="K", serialNumber="101", orderId=order,
        orderUpdateId=0, lastNodeId="A", lastNodeSequenceId=0, nodeStates=[], edgeStates=[],
        driving=False, actionStates=[], errors=[], operatingMode="AUTOMATIC",
        powerSupply=dict(stateOfCharge=80, charging=False),
        safetyState=dict(activeEmergencyStop="NONE", fieldViolation=False),
        instantActionStates=[dict(actionId=token, actionType="stateRequest", actionStatus="FINISHED")]))


class FinalStorage(Storage):
    def __init__(self, snapshot):
        super().__init__(snapshot)
        self.records = []
        self.called = threading.Event()
        self.release = threading.Event()

    def SaveMissionExecution(self, request, context):
        self.records.append(request.execution.SerializeToString(deterministic=True))
        self.called.set()
        if len(self.records) == 1:
            context.abort(grpc.StatusCode.UNAVAILABLE, "lost checkpoint reply")
        if not self.release.wait(4):
            context.abort(grpc.StatusCode.UNAVAILABLE, "waiting for test")
        return db.SaveMissionExecutionResponse()


@pytest.mark.parametrize("order,result,later", [
    ("m-s", "Attach", False), ("foreign", "Conflict", False), ("m-s", "Attach", True),
])
def test_restart_requires_current_probe(tmp_path, order, result, later):
    port = free_port()
    cfg = tmp_path / "mosquitto.conf"
    cfg.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    with process(["mosquitto", "-c", str(cfg)], tmp_path / "mqtt.log") as broker, ThreadPoolExecutor(4) as pool:
        snapshot = checkpoint()
        head = snapshot.active_missions[0]
        if later:
            head.mission.nodes[0].move.nodes[0].actions.add(
                action_id="scan", action_type="scan", blocking_type=fms.BLOCKING_TYPE_HARD)
            head.status.steps[0].actions.add(action_id="scan", action_type="scan")
            payload = json.loads(head.order.sent_order_json)
            payload["nodes"][0]["actions"] = [dict(actionId="scan", actionType="scan", blockingType="HARD")]
            head.order.sent_order_json = json.dumps(payload)
        snapshot.last_grants[0].CopyFrom(head)
        head.ClearField("order")
        head.checkpoint_version = 8
        store = FinalStorage(snapshot)
        store.available.set()
        server = grpc.server(pool)
        db_port = server.add_insecure_port("127.0.0.1:0")
        db_rpc.add_DispatchStorageServicer_to_server(store, server)
        server.start()
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        sub = threading.Event()
        tokens, orders = queue.Queue(), queue.Queue()

        def message(_, __, msg):
            if msg.topic.endswith("/order"):
                orders.put(msg.payload)
            elif msg.topic.endswith("/instantActions"):
                for a in json.loads(msg.payload)["actions"]:
                    tokens.put((a["actionType"], a["actionId"]))

        def token():
            kind, action_id = tokens.get(timeout=18)
            assert kind == "stateRequest"
            return action_id

        client.on_message = message
        client.on_subscribe = lambda c, u, m, codes, p: sub.set() if all(not x.is_failure for x in codes) else None
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    client.connect("127.0.0.1", port)
                    break
                except OSError:
                    if time.monotonic() >= deadline or broker.poll() is not None:
                        raise
                    time.sleep(.05)
            client.loop_start()
            client.subscribe(TOPIC + "#", qos=1)
            assert sub.wait(5)
            addr = f"127.0.0.1:{free_port()}"
            env = dict(os.environ, DATABASE_ADDRESS=f"127.0.0.1:{db_port}", MQTT_HOST="127.0.0.1",
                       MQTT_PORT=str(port), DISPATCH_LISTEN_ADDRESS=addr, MQTT_USER="", MQTT_PASSWORD="")
            binary = ROOT / "cmake-build-debug/fleet_dispatch/fleet_dispatch"
            with process(["stdbuf", "-oL", binary], tmp_path / "first.log", env):
                old = token()
            log = tmp_path / "second.log"
            with process(["stdbuf", "-oL", binary], log, env), grpc.insecure_channel(addr) as channel:
                current = token()
                assert current != old
                client.publish(TOPIC + "state", state(old, "foreign", 1), qos=1).wait_for_publish(3)
                # An old response must neither attach nor permanently conflict. Expect a retry.
                retry = token()
                assert retry not in (old, current)
                api = dp_rpc.FleetDispatchStub(channel)
                reply = json.loads(state(retry, order, 2))
                if later:
                    reply["actionStates"] = [dict(actionId="scan", actionType="scan", actionStatus="RUNNING")]
                def publish():
                    client.publish(TOPIC + "state", json.dumps(reply), qos=1).wait_for_publish(3)
                publish()
                until = time.monotonic() + 4
                while time.monotonic() < until and f"result={result} " not in log.read_text():
                    time.sleep(.05)
                assert f"result={result} " in log.read_text(), log.read_text()
                assert f"result={'Conflict' if result == 'Attach' else 'Attach'} " not in log.read_text()
                rejected(first, api.StreamMissionStatus, dp.StreamMissionStatusRequest())
                def owner():
                    update = first(api.StreamRobotStatus, dp.StreamRobotStatusRequest())
                    robot, = update.snapshot.robots
                    return robot.mission_id

                assert owner() == "m"
                if later:
                    assert not store.called.is_set()
                    reply["instantActionStates"] = []
                    reply["actionStates"][0]["actionStatus"] = "FINISHED"
                    reply["headerId"] += 1
                    publish()
                if result == "Attach":
                    assert store.called.wait(4), log.read_text()
                    publish()
                    assert owner() == "m"
                    store.release.set()
                    until = time.monotonic() + 8
                    while owner() and time.monotonic() < until:
                        time.sleep(.05)
                    assert owner() == "", log.read_text()
                    assert len(store.records) == 2 and store.records[0] == store.records[1]
                    saved = db.MissionExecution.FromString(store.records[0])
                    assert saved.checkpoint_version == 9
                    assert saved.status.state == fms.MISSION_STATE_COMPLETED
                    assert saved.status.mission_id == "m" and not saved.HasField("order")
                else:
                    assert not store.called.is_set()
                with pytest.raises(queue.Empty):
                    orders.get(timeout=.5)
        finally:
            client.disconnect()
            client.loop_stop()
            server.stop(0).wait()
