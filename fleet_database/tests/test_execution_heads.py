import uuid

import grpc
import pytest

import fleet_database_pb2 as db
import fms_types_pb2 as fms
from conftest import error_code


def load(storage):
    return storage.LoadDispatchSnapshot(db.LoadDispatchSnapshotRequest(), timeout=5).snapshot


def test_claim_wait_cancel_and_late_write(storage, robots):
    spec = robots.create()
    mid = "head-" + uuid.uuid4().hex
    storage.DispatchAddReceipt(db.DispatchAddReceiptRequest(
        operation=db.DispatchAddReceiptRequest.CLAIM, mission_id=mid, request_bytes=b"opaque-add",
    ), timeout=5)
    snap = load(storage)
    assert any(r.spec.robot_id == spec.robot_id for r in snap.robots)
    unknown, = (u for u in snap.unresolved if u.mission_id == mid)
    assert unknown.add_request == b"opaque-add"
    req = db.SaveMissionExecutionRequest()
    e = req.execution
    e.checkpoint_version = 1
    e.mission.mission_id = e.status.mission_id = mid
    e.mission.robot_id = e.status.robot_id = spec.robot_id
    e.mission.nodes.add(id="wait").wait.duration.seconds = 10
    e.status.current_node_id = "wait"
    e.status.steps.add(node_id="wait", attempts=1, state=fms.STEP_STATE_RUNNING)
    e.status.state = fms.MISSION_STATE_RUNNING
    storage.SaveMissionExecution(req, timeout=5)
    storage.SaveMissionExecution(req, timeout=5)
    assert e in load(storage).active_missions
    old = db.SaveMissionExecutionRequest.FromString(req.SerializeToString())
    e.checkpoint_version = 2
    e.cancel_action_id = "cancel-" + mid
    e.status.state = fms.MISSION_STATE_CANCELING
    storage.SaveMissionExecution(req, timeout=5)
    assert e in load(storage).active_missions
    assert error_code(storage.SaveMissionExecution, old) == grpc.StatusCode.FAILED_PRECONDITION
    e.status.ClearField("current_node_id")
    e.status.state = fms.MISSION_STATE_CANCELED
    e.status.steps[0].state = fms.STEP_STATE_CANCELED
    e.checkpoint_version = 3
    storage.SaveMissionExecution(req, timeout=5)
    snap = load(storage)
    assert all(m.mission.mission_id != mid for m in snap.active_missions)
    assert any(u.mission_id == mid and u.last_status == e for u in snap.unresolved)
    old.execution.checkpoint_version = 4
    assert error_code(storage.SaveMissionExecution, old) == grpc.StatusCode.FAILED_PRECONDITION
