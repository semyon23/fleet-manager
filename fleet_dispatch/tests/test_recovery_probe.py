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


@pytest.mark.parametrize("order,result", [("m-s", "Attach"), ("foreign", "Conflict")])
def test_restart_requires_current_probe(tmp_path, order, result):
    port = free_port()
    cfg = tmp_path / "mosquitto.conf"
    cfg.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    with process(["mosquitto", "-c", str(cfg)], tmp_path / "mqtt.log") as broker, ThreadPoolExecutor(4) as pool:
        store = Storage(checkpoint())
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
                client.publish(TOPIC + "state", state(retry, order, 2), qos=1).wait_for_publish(3)
                until = time.monotonic() + 4
                while time.monotonic() < until and f"result={result} " not in log.read_text():
                    time.sleep(.05)
                assert f"result={result} " in log.read_text(), log.read_text()
                assert f"result={'Conflict' if result == 'Attach' else 'Attach'} " not in log.read_text()
                rejected(first, api.StreamMissionStatus, dp.StreamMissionStatusRequest())
                with pytest.raises(queue.Empty):
                    orders.get(timeout=.5)
        finally:
            client.disconnect()
            client.loop_stop()
            server.stop(0).wait()
