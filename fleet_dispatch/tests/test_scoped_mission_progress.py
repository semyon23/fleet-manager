"""The real control process reads one live route while the mission stream stays closed."""
import json
import os
import threading
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from urllib.request import urlopen

import grpc
import pytest

from test_control_recovery_assessment import Dispatch, Storage
from test_dispatch_startup import ROOT, db_rpc, dp, dp_rpc
from test_move_restart_stand import wait
from test_recovery_probe import free_port, process


class ProgressDispatch(Dispatch):
    def __init__(self, storage):
        super().__init__(storage)
        self.observations = []
        self.fault = ""
        self.pending = None

    def GetMission(self, request, context):
        with self.lock:
            identity = (request.mission_id, request.expected_robot_id, request.expected_instance_id,
                        request.expected_add_attempt if request.HasField("expected_add_attempt") else None)
            self.observations.append(identity)
            pending, fault = self.pending, self.fault
        if identity[:2] != ("trip", "A") or not identity[2] or identity[3] != 0:
            context.abort(grpc.StatusCode.UNAVAILABLE, "unscoped or wrong mission")
        reply = dp.GetMissionResponse(instance_id=identity[2], add_attempt=0,
                                      mission=self.cp.executed_mission, status=self.cp.status)
        reply.status.updated_at.GetCurrentTime()
        if pending:
            entered, release, returned = pending
            entered.set()
            while not release.wait(.02):
                if not context.is_active():
                    context.abort(grpc.StatusCode.CANCELLED, "expired scoped call")
        if fault == "attempt":
            reply.add_attempt = 1
        elif fault == "absent":
            reply.ClearField("add_attempt")
        elif fault == "source":
            reply.instance_id = "previous-dispatch"
        elif fault == "path":
            reply.mission.nodes[0].move.edges[0].edge_id = "EC"
        elif fault == "unavailable":
            context.abort(grpc.StatusCode.UNAVAILABLE, "injected scoped read failure")
        if pending:
            if not context.is_active():
                context.abort(grpc.StatusCode.CANCELLED, "expired scoped call")
            returned.set()
        return reply


@contextmanager
def scoped_stand(tmp_path):
    storage = Storage()
    dispatch = ProgressDispatch(storage)
    dispatch.publish()
    with ThreadPoolExecutor(12) as pool:
        server = grpc.server(pool)
        db_rpc.add_RobotRegistryServicer_to_server(storage, server)
        db_rpc.add_ControlStorageServicer_to_server(storage, server)
        dp_rpc.add_FleetDispatchServicer_to_server(dispatch, server)
        port = server.add_insecure_port("127.0.0.1:0")
        server.start()
        try:
            address = f"127.0.0.1:{port}"
            http = f"127.0.0.1:{free_port()}"
            env = dict(os.environ, DISPATCH_ADDRESS=address, DATABASE_ADDRESS=address, HTTP_ADDRESS=http)
            log = tmp_path / "control.log"
            with process([ROOT / "cmake-build-debug/fleet_control/fleet_control"], log, env) as control:
                def read(path):
                    assert control.poll() is None, log.read_text()
                    with urlopen(f"http://{http}/api/fms/routes{path}", timeout=2) as response:
                        return json.load(response)

                def routes():
                    return [r for r in read("?mapId=map")["items"]
                            if r["missionId"] == "trip" and r["progressSource"] == "scoped_read"]

                def current_revision():
                    return read("/recovery-assessment?missionId=free&robotId=D")["revision"]
                assert dispatch.closed_stream.wait(12), log.read_text()
                yield dispatch, routes, current_revision
            assert storage.writes == [] and dispatch.motion == []
        finally:
            server.stop(0).wait()


def expected_route(routes):
    rows = routes()
    if not rows:
        return False
    row, = rows
    assert row["robotId"] == "A" and row["stepIndex"] == 0
    assert row["released"] == {"nodes": ["B", "C"], "edges": ["BC"]}
    return True


@pytest.mark.parametrize("fault", ["attempt", "absent", "source", "path", "unavailable"])
def test_only_verified_narrow_reply_updates_route(tmp_path, fault):
    with scoped_stand(tmp_path) as (dispatch, routes, _):
        wait(lambda: expected_route(routes), "verified B-C route", 20)
        with dispatch.lock:
            assert ("trip", "A", "first", 0) in dispatch.observations
            before = len(dispatch.observations)
            dispatch.fault = fault
        wait(lambda: len(dispatch.observations) > before and routes() == [], "withdraw unverifiable route", 18)
        with dispatch.lock:
            dispatch.fault = ""
        wait(lambda: expected_route(routes), "refresh the same active route", 20)


def test_inflight_reply_does_not_cross_inventory_revision(tmp_path):
    with scoped_stand(tmp_path) as (dispatch, routes, current_revision):
        wait(lambda: expected_route(routes), "first observed route", 20)
        old_read, release_old, returned_old = threading.Event(), threading.Event(), threading.Event()
        new_read, release_new = threading.Event(), threading.Event()
        try:
            with dispatch.lock:
                dispatch.pending = old_read, release_old, returned_old
            assert old_read.wait(12)
            with dispatch.lock:
                dispatch.pending = new_read, release_new, threading.Event()
            dispatch.publish(revision=2)
            # Wait until the new inventory has actually replaced the old one in control.
            wait(lambda: current_revision() == 2 and routes() == [], "discard prior inventory revision", 8)
            release_old.set()
            assert returned_old.wait(1), "old response must succeed within the gRPC deadline"
            assert new_read.wait(12)
            assert routes() == []
            release_new.set()
            wait(lambda: expected_route(routes), "observed under the new revision", 20)
        finally:
            release_old.set()
            release_new.set()
