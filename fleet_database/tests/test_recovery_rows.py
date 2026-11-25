"""Компактный снимок незакрытых исполнений через настоящий DispatchStorage."""

import json
import pytest

import fleet_database_pb2 as db
import fleet_dispatch_pb2 as dispatch
import fms_types_pb2 as fms
from conftest import unique


def load(storage):
    response = storage.LoadDispatchSnapshot(db.LoadDispatchSnapshotRequest(recovery_only=True), timeout=5)
    assert response.HasField("recovery")
    assert not response.HasField("snapshot")
    return response.recovery


def read_row(storage, mid):
    row, = (r for r in load(storage).missions if r.mission_id == mid)
    return row


def checkpoint(storage, mid, rid, *, completed=False):
    request = db.SaveMissionExecutionRequest()
    execution = request.execution
    execution.checkpoint_version = 2 if completed else 1
    execution.mission.mission_id = execution.status.mission_id = mid
    execution.mission.robot_id = execution.status.robot_id = rid
    execution.mission.nodes.add(id="wait").wait.duration.seconds = 10
    step = execution.status.steps.add(node_id="wait", attempts=1)
    if completed:
        execution.status.state = fms.MISSION_STATE_COMPLETED
        step.state = fms.STEP_STATE_COMPLETED
    else:
        execution.status.state = fms.MISSION_STATE_RUNNING
        execution.status.current_node_id = "wait"
        step.state = fms.STEP_STATE_RUNNING
    storage.SaveMissionExecution(request, timeout=5)


@pytest.fixture
def identity(robots, storage):
    mid, rid = unique("test-recovery-row"), robots.create().robot_id
    yield mid, rid
    # Закрываем свою запись, чтобы она не осталась в стартовом инвентаре.
    checkpoint(storage, mid, rid, completed=True)


def add_bytes(mid, rid, attempt):
    return dispatch.AddMissionRequest(mission=fms.Mission(mission_id=mid, robot_id=rid),
                                      attempt=attempt).SerializeToString()


def claim(storage, mid, attempt, payload):
    result = storage.DispatchAddReceipt(db.DispatchAddReceiptRequest(
        operation=db.DispatchAddReceiptRequest.CLAIM, mission_id=mid,
        attempt=attempt, request_bytes=payload,
    ), timeout=5)
    assert result.state == db.DispatchAddReceiptResponse.CLAIMED_NOW


@pytest.mark.parametrize("attempt", [0, 7])
def test_receipt_owner_and_attempt_through_completion(storage, robots, identity, attempt):
    mid, rid = identity
    claim(storage, mid, attempt, add_bytes(mid, rid, attempt))
    robots.set_deleting(rid)
    membership = load(storage)
    assert rid in membership.robots and rid in membership.deleting
    pending = read_row(storage, mid)
    assert pending.robot_id == rid and pending.uncertain and not pending.conflict
    assert pending.HasField("add_attempt") and pending.add_attempt == attempt

    checkpoint(storage, mid, rid)
    running = read_row(storage, mid)
    assert running.robot_id == rid and not running.uncertain and not running.conflict
    assert running.HasField("add_attempt") and running.add_attempt == attempt

    checkpoint(storage, mid, rid, completed=True)
    assert mid not in {r.mission_id for r in load(storage).missions}


def test_active_head_without_receipt_has_no_attempt(storage, identity):
    mid, rid = identity
    checkpoint(storage, mid, rid)
    row = read_row(storage, mid)
    assert row.robot_id == rid and not row.HasField("add_attempt") and not row.conflict
    original = storage.LoadDispatchSnapshot(db.LoadDispatchSnapshotRequest(), timeout=5)
    assert original.HasField("snapshot") and not original.HasField("recovery")
    assert any(e.mission.mission_id == mid for e in original.snapshot.active_missions)


@pytest.mark.parametrize("bad_receipt", ["unreadable", "mission", "attempt", "owner"])
def test_conflicting_receipt_stays_in_snapshot(storage, identity, bad_receipt):
    mid, rid = identity
    attempt = 2
    payload = {
        "unreadable": b"opaque-add",
        "mission": add_bytes("different-mission", rid, attempt),
        "attempt": add_bytes(mid, rid, attempt + 1),
        "owner": add_bytes(mid, "different-robot", attempt),
    }[bad_receipt]
    claim(storage, mid, attempt, payload)
    if bad_receipt == "unreadable":
        row = read_row(storage, mid)
        assert row.robot_id == "" and row.conflict and row.uncertain
        assert not row.HasField("add_attempt")

    checkpoint(storage, mid, rid)
    row = read_row(storage, mid)
    assert row.robot_id == rid and row.conflict and row.uncertain
    assert not row.HasField("add_attempt")


def test_legacy_orders_disagree_on_owner(storage, robots, identity):
    mid, rid = identity
    claim(storage, mid, 0, add_bytes(mid, rid, 0))
    for owner in (rid, robots.create().robot_id):
        execution = db.MissionExecution()
        execution.mission.mission_id = execution.status.mission_id = mid
        execution.mission.robot_id = execution.status.robot_id = owner
        execution.mission.nodes.add(id="s").action.SetInParent()
        execution.status.current_node_id = "s"
        execution.status.state = fms.MISSION_STATE_RUNNING
        execution.status.steps.add(node_id="s", state=fms.STEP_STATE_RUNNING)
        order = execution.order
        order.order_id, order.mission_node_id = "order-" + owner, "s"
        order.sent_order_json = json.dumps(dict(orderId=order.order_id, orderUpdateId=0,
                                               nodes=[dict(sequenceId=0, released=True)], edges=[]))
        storage.SaveMissionExecution(db.SaveMissionExecutionRequest(execution=execution), timeout=5)

    row = read_row(storage, mid)
    assert row.robot_id == "" and row.conflict and row.uncertain
    assert not row.HasField("add_attempt")
