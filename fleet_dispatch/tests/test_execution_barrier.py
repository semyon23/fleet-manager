"""Integration tests on an isolated PostgreSQL database and the robot simulator."""
import json
import os
import queue
import threading
import time
from contextlib import contextmanager

import grpc
import paho.mqtt.client as mqtt
import pytest

import fleet_database_pb2 as db
import fleet_database_pb2_grpc as dbgrpc
import fleet_dispatch_pb2 as dispatch
import fms_types_pb2 as fms
from conftest import ORIGIN
from dispatch_client import (
    is_final, mission_of, mission_status, move_mission, new_id, node,
    step_action, step_wait, unknown_action,
)

pytestmark = pytest.mark.skipif(
    os.getenv("EXECUTION_BARRIER_TESTS") != "1", reason="requires an isolated stand",
)


@pytest.fixture
def sql():
    import psycopg

    with psycopg.connect(os.environ["DATABASE_URL"], autocommit=True, connect_timeout=5) as connection:
        yield connection


@pytest.fixture
def orders(settings):
    target = settings.target
    messages = queue.Queue()
    subscribed = threading.Event()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if target.username:
        client.username_pw_set(target.username, target.password)

    def on_subscribe(client, userdata, mid, reasons, properties):
        if reasons and all(not reason.is_failure for reason in reasons):
            subscribed.set()

    client.on_subscribe = on_subscribe
    client.on_message = lambda client, userdata, msg: messages.put(msg.payload)
    client.connect(target.host, target.port)
    client.loop_start()
    try:
        rc, _ = client.subscribe(target.topic("order"), qos=1)
        assert rc == mqtt.MQTT_ERR_SUCCESS and subscribed.wait(5), "MQTT subscription failed"
        yield messages
    finally:
        client.disconnect()
        client.loop_stop()


@contextmanager
def block_writes(sql):
    import psycopg

    with psycopg.connect(os.environ["DATABASE_URL"], connect_timeout=5) as holder:
        holder.execute("SET LOCAL lock_timeout = '5s'")
        holder.execute("SET LOCAL idle_in_transaction_session_timeout = '45s'")
        holder.execute("LOCK TABLE dispatch_execution_orders IN ACCESS EXCLUSIVE MODE")
        pid = holder.info.backend_pid

        def wait_for_writer():
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                blocked = sql.execute(
                    "SELECT EXISTS (SELECT 1 FROM pg_locks WHERE NOT granted "
                    "AND relation = 'dispatch_execution_orders'::regclass "
                    "AND mode = 'RowExclusiveLock' AND %s = ANY(pg_blocking_pids(pid)))",
                    (pid,),
                ).fetchone()[0]
                if blocked:
                    return
                time.sleep(0.05)
            pytest.fail("fleet_database did not attempt the execution write in this database")

        yield wait_for_writer


def observe(messages, order_id, update, timeout):
    deadline = time.monotonic() + timeout
    while True:
        raw = messages.get(timeout=max(0, deadline - time.monotonic()))
        doc = json.loads(raw)
        if (doc["orderId"], doc["orderUpdateId"]) == (order_id, update):
            return raw


def snapshot(sql, mid, order_id, update):
    rows = sql.execute(
        "SELECT execution FROM dispatch_execution_orders "
        "WHERE mission_id = %s AND order_id = %s AND update_id = %s",
        (mid, order_id, update),
    ).fetchall()
    assert len(rows) == 1
    return db.MissionExecution.FromString(bytes(rows[0][0]))


@pytest.mark.parametrize("case", ["initial", "next_step", "extend"])
def test_execution_commits_before_mqtt(sql, orders, robot, case):
    mid = new_id("execution")
    mission = move_mission(mid, robot.robot_id, [
        node(ORIGIN, 0), node("a", 1), node("b", 2),
    ], released_nodes=2)
    if case == "next_step":
        mission = mission_of(mid, robot.robot_id, [step_wait("wait", 1), mission.nodes[0]])

    order_id = mid + "-s1"
    update = int(case == "extend")
    if case == "extend":
        robot.add_mission(mission)
        observe(orders, order_id, 0, 10)
    else:
        robot.missions.append(mid)

    with block_writes(sql) as wait_for_writer:
        if case == "extend":
            call = robot.dispatch.ExtendMission.future(dispatch.ExtendMissionRequest(
                mission_id=mid, move_node_id="s1", revision=1, released_nodes=3,
            ), timeout=30)
        else:
            call = robot.dispatch.AddMission.future(dispatch.AddMissionRequest(mission=mission), timeout=30)
        wait_for_writer()
        # Longer than DatabaseWriter's three-second RPC deadline. The retry must
        # reuse the same snapshot even if the first transaction eventually commits.
        with pytest.raises(queue.Empty):
            observe(orders, order_id, update, 4)

    call.result(timeout=15)
    raw = observe(orders, order_id, update, 10)
    saved = snapshot(sql, mid, order_id, update)
    step = saved.status.steps[-1]
    assert saved.order.sent_order_json.encode() == raw
    assert saved.order.mission_node_id == step.node_id == "s1"
    assert saved.mission.robot_id == robot.robot_id
    assert saved.mission.nodes[-1].move.released_nodes == step.move.released_nodes == 2 + update
    assert step.move.revision == update
    assert not saved.order.confirmed and not saved.order.HasField("sent_at")

    with grpc.insecure_channel(robot.settings.database_address) as channel:
        storage = dbgrpc.DispatchStorageStub(channel)
        request = db.SaveMissionExecutionRequest(execution=saved)
        storage.SaveMissionExecution(request, timeout=5)
        request.execution.order.sent_order_json += " "
        with pytest.raises(grpc.RpcError) as error:
            storage.SaveMissionExecution(request, timeout=5)
        assert error.value.code() == grpc.StatusCode.ALREADY_EXISTS
        assert snapshot(sql, mid, order_id, update) == saved


def test_retry_has_separate_execution(sql, orders, robot):
    mid = new_id("execution-retry")
    stream = robot.mission_updates()
    robot.add_mission(mission_of(mid, robot.robot_id, [
        step_action("bad", [unknown_action()], on_failure=fms.FAILURE_ACTION_RETRY, max_retries=1),
    ]))
    final = stream.wait(mission_status(mid, is_final), 30, what="retried mission failed").status
    assert final.state == fms.MISSION_STATE_FAILED
    for attempt, suffix in [(1, ""), (2, "-a2")]:
        order_id = mid + "-bad" + suffix
        raw = observe(orders, order_id, 0, 5)
        saved = snapshot(sql, mid, order_id, 0)
        assert saved.order.sent_order_json.encode() == raw
        assert saved.status.steps[0].attempts == attempt


def test_cancel_during_write_suppresses_order(sql, orders, robot):
    mid = new_id("execution-cancel")
    mission = move_mission(mid, robot.robot_id, [node(ORIGIN, 0), node("a", 1)])
    robot.missions.append(mid)
    with block_writes(sql) as wait_for_writer:
        call = robot.dispatch.AddMission.future(dispatch.AddMissionRequest(mission=mission), timeout=25)
        wait_for_writer()
        robot.dispatch.CancelMission(dispatch.CancelMissionRequest(mission_id=mid), timeout=5)
    with pytest.raises(grpc.RpcError) as error:
        call.result(timeout=10)
    assert error.value.code() == grpc.StatusCode.UNAVAILABLE
    with pytest.raises(queue.Empty):
        observe(orders, mid + "-s1", 0, 3)
