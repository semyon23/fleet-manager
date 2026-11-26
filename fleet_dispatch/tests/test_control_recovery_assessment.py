"""Real fleet_control with isolated storage and dispatch. Motion RPCs are recorded."""
import json
import os
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import urlopen

import grpc
import pytest

from test_dispatch_startup import ROOT, db, db_rpc, dp, dp_rpc, fms
from test_move_restart_stand import wait
from test_recovery_probe import free_port, process

Inventory = dp.GetRecoveryInventoryResponse
Checkpoint = fms.GetExecutionCheckpointResponse
STAMP = "2026-01-01T00:00:00.000Z"
POINTS = {"B": (0, 0), "C": (8, 0), "E": (0, 8), "F": (8, 8)}
EDGES = ["BC", "CB", "EF", "EC"]


def mission_record(name, robot, goal, running=False):
    state = "RUNNING" if running else "QUEUED"
    doc = dict(id=name, name=name, mapId="map", requestedRobotId=robot,
               robotId=robot if running else None, priority="NORMAL", state=state,
               steps=[dict(nodeId=goal, actions=[])], currentStep=0 if running else None,
               stepStates=[dict(state="RUNNING" if running else "PENDING", actions=[],
                                startedAt=STAMP if running else None, finishedAt=None,
                                waitingForTraffic=False)], error=None, createdAt=STAMP,
               assignedAt=STAMP if running else None, startedAt=STAMP if running else None,
               finishedAt=None, updatedAt=STAMP)
    return db.MissionRecord(mission_id=name, robot_id=robot if running else "", map_id="map",
                            priority=1, state=state, body_json=json.dumps(doc))


def moving_trip(document):
    add = dp.AddMissionRequest(attempt=0)
    add.mission.mission_id, add.mission.robot_id = "trip", "A"
    move = add.mission.nodes.add(id="s0", name="C").move
    move.released_nodes = 1
    for node in ("B", "C"):
        x, y = POINTS[node]
        move.nodes.add(node_id=node, map_id="map", x=x, y=y)
    move.edges.add(edge_id="BC")

    def command(kind, command_id, req, nodes, edges, **fields):
        snap = dict(version=1, attempt=0, mapVersion=7, routes=[["B", "C"]], edges=[["BC"]],
                    mission=json.loads(document.body_json),
                    protectedBase=dict(robot="A", key="trip/0/s0", map="map", version=7,
                                       nodes=nodes, edges=edges, reached=0))
        row = db.ControlCommandRecord(command_id=command_id, robot_id="A", mission_id="trip",
                                      map_id="map", map_version=7, kind=kind, recovery_version=1,
                                      request_proto=req.SerializeToString(), recovery_json=json.dumps(snap),
                                      **fields)
        return db.StoredControlCommand(command=row)

    extend = dp.ExtendMissionRequest(mission_id="trip", move_node_id="s0", revision=1, released_nodes=2)
    rows = [command(db.CONTROL_COMMAND_KIND_ADD_MISSION, "add", add, [0], []),
            command(db.CONTROL_COMMAND_KIND_EXTEND_MISSION, "extend", extend, [0, 1], [0],
                    move_node_id="s0", revision=1)]
    cp = Checkpoint(kind=Checkpoint.ACTIVE, mission_id="trip", robot_id="A", add_attempt=0,
                    checkpoint_version=2, accepted_add=add.SerializeToString(),
                    executed_mission=add.mission, grant_step="s0", grant_order="trip-s0", grant_update=1)
    cp.executed_mission.nodes[0].move.released_nodes = 2
    cp.status.mission_id, cp.status.robot_id = "trip", "A"
    cp.status.current_node_id, cp.status.state = "s0", fms.MISSION_STATE_RUNNING
    step = cp.status.steps.add(node_id="s0", state=fms.STEP_STATE_RUNNING, attempts=1)
    step.move.total_nodes = step.move.released_nodes = 2
    step.move.last_node_id, step.move.revision, step.move.confirmed_revision = "B", 1, 1
    return rows, cp


def live_robot(robot):
    busy = robot == "A"
    node = "B" if busy else "E"
    status = fms.RobotStatus(robot_id=robot, connection=fms.CONNECTION_STATE_ONLINE,
                             localized=True, operating_mode=fms.OPERATING_MODE_AUTOMATIC,
                             last_node_id=node, driving=busy, mission_id="trip" if busy else "")
    x, y = POINTS[node]
    status.pose.x, status.pose.y, status.pose.map_id = (x + 3 if busy else x), y, "map"
    status.safety.emergency_stop = fms.EMERGENCY_STOP_NONE
    status.power.state_of_charge = 90
    status.observed_order.idle = not busy
    if busy:
        status.observed_order.order_id, status.observed_order.update_id = "trip-s0", 1
    status.factsheet_received_at.seconds = 1
    status.last_state_at.GetCurrentTime()
    return status


class Storage(db_rpc.ControlStorageServicer, db_rpc.RobotRegistryServicer):
    def __init__(self):
        self.trip = mission_record("trip", "A", "C", running=True)
        self.rows, self.checkpoint = moving_trip(self.trip)
        self.writes = []

    def ListMaps(self, request, context):
        layout = dict(width=12, height=12, meta=dict(resolution=1, origin=[0, 0, 0]),
                      waypoints=[dict(id=name, u=x, v=12-y) for name, (x, y) in POINTS.items()],
                      edges=[{"id": edge, "from": edge[0], "to": edge[1]} for edge in EDGES],
                      stations=[], zones=[], assignedRobots=[])
        return db.ListMapsResponse(maps=[db.MapRecord(map_id="map", name="Map", version=7,
                                                       body_json=json.dumps(layout))])

    def ListRobots(self, request, context):
        return db.ListRobotsResponse(robots=[
            db.RobotRecord(spec=db.RobotSpec(robot_id=name, manufacturer="K", serial_number=name,
                                             lifecycle=db.ROBOT_LIFECYCLE_ACTIVE)) for name in ("A", "D")])

    def ListActiveMissions(self, request, context):
        return db.ListActiveMissionsResponse(missions=[self.trip, mission_record("free", "D", "F"),
                                                       mission_record("cross", "D", "C")])

    def GetMission(self, request, context):
        if request.mission_id != "trip":
            context.abort(grpc.StatusCode.NOT_FOUND, request.mission_id)
        return db.GetMissionResponse(mission=self.trip)

    def ListOpenControlMissions(self, request, context):
        return db.ListOpenControlMissionsResponse(missions=[db.OpenControlMission(mission_id="trip")])

    def ListControlCommands(self, request, context):
        return db.ListControlCommandsResponse(commands=self.rows if request.mission_id in ("", "trip") else [])

    def PutControlCommand(self, request, context):
        self.writes.append(request.command.command_id)
        return db.PutControlCommandResponse()

    def ListOpenAlarms(self, request, context):
        return db.ListOpenAlarmsResponse()

    def SaveRobotAvailability(self, request, context):
        return db.SaveRobotAvailabilityResponse()

    def AppendEvents(self, request, context):
        return db.AppendEventsResponse()


class Dispatch(dp_rpc.FleetDispatchServicer):
    def __init__(self, storage):
        self.cp = storage.checkpoint
        self.lock = threading.Lock()
        self.reply = None
        self.hold = None
        self.checkpoint_hold = None
        self.calls = self.inflight = self.peak = 0
        self.motion = []
        self.closed_stream = threading.Event()
        self.next_stream = None
        self.robot_instance = "first"
        self.robot_stream_instance = None

    def publish(self, instance="first", revision=1, state=Inventory.READY, attempt=0, hold=False,
                restricted=(), update_robots=True):
        with self.lock:
            if update_robots:
                self.robot_instance = instance
            mission = dp.RecoveryMission(mission_id="trip", robot_id="A")
            if attempt is not None:
                mission.add_attempt = attempt
            self.reply = Inventory(state=state, instance_id=instance, revision=revision)
            if state != Inventory.LOADING:
                self.reply.robots.extend(["A", "D"])
                self.reply.blocked_robots.append("A")
                self.reply.missions.add().CopyFrom(mission)
                self.reply.live_restrictions.SetInParent()
                self.reply.live_restrictions.robots.extend(restricted)
            self.hold = (threading.Event(), threading.Event()) if hold else None
            return self.hold

    def GetRecoveryInventory(self, request, context):
        with self.lock:
            self.calls += 1
            self.inflight += 1
            self.peak = max(self.peak, self.inflight)
            reply, hold = self.reply, self.hold
        try:
            if hold:
                entered, release = hold
                entered.set()
                while context.is_active() and not release.wait(.02):
                    pass
            if reply is None:
                context.abort(grpc.StatusCode.UNAVAILABLE, "database disconnected")
            return reply
        finally:
            with self.lock:
                self.inflight -= 1

    def StreamRobotStatus(self, request, context):
        done = threading.Event()
        context.add_callback(done.set)
        with self.lock:
            instance = self.robot_instance
            self.robot_stream_instance = instance
        yield dp.RobotStatusUpdate(snapshot=dp.RobotSnapshot(
            instance_id=instance, robots=[live_robot("A"), live_robot("D")]))
        while not done.wait(.15):
            with self.lock:
                restart = instance != self.robot_instance
            if restart:
                context.abort(grpc.StatusCode.UNAVAILABLE, "robot stream from previous dispatch closed")
            for robot in ("A", "D"):
                yield dp.RobotStatusUpdate(status=live_robot(robot))

    def StreamMissionStatus(self, request, context):
        with self.lock:
            handshake, self.next_stream = self.next_stream, None
            instance = self.reply.instance_id if self.reply else self.robot_instance
        if handshake:
            connected, send_snapshot = handshake
            connected.set()
            while context.is_active() and not send_snapshot.wait(.02):
                pass
            if context.is_active():
                # A missing active mission stays protected; opening this empty snapshot
                # and immediately disconnecting creates a real stream epoch change.
                yield dp.MissionStatusUpdate(snapshot=dp.MissionSnapshot(instance_id=instance))
        self.closed_stream.set()
        context.abort(grpc.StatusCode.UNAVAILABLE, "mission stream is gated at startup")

    def GetExecutionCheckpoint(self, request, context):
        if request.mission_id != "trip":
            context.abort(grpc.StatusCode.NOT_FOUND, request.mission_id)
        with self.lock:
            hold = self.checkpoint_hold
        if hold:
            entered, release, returned = hold
            entered.set()
            while not release.wait(.02):
                if not context.is_active():
                    context.abort(grpc.StatusCode.CANCELLED, "checkpoint reader gone")
            if not context.is_active():
                context.abort(grpc.StatusCode.CANCELLED, "checkpoint reader gone")
            returned.set()
        return self.cp

    def AddMission(self, request, context):
        self.motion.append("AddMission")
        return dp.AddMissionResponse()

    def ExtendMission(self, request, context):
        self.motion.append("ExtendMission")
        return dp.ExtendMissionResponse()


@pytest.fixture
def assessment(tmp_path):
    storage = Storage()
    dispatch = Dispatch(storage)
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
                def query(mission="free", robot="D"):
                    assert control.poll() is None, log.read_text()
                    args = urlencode(dict(missionId=mission, robotId=robot))
                    url = f"http://{http}/api/fms/routes/recovery-assessment?{args}"
                    try:
                        with urlopen(url, timeout=2) as response:
                            return json.load(response)
                    except HTTPError as error:
                        pytest.fail(f"assessment returned {error.code}: {error.read()!r}")
                assert dispatch.closed_stream.wait(12), log.read_text()
                wait(lambda: dispatch.calls > 0, "poll inventory with mission stream gated")
                yield dispatch, query
                assert dispatch.motion == []
                assert storage.writes == []
        finally:
            server.stop(0).wait()


def await_view(query, granted, instance, revision, mission="free", reason=None):
    expected = granted, instance, revision
    deadline = time.monotonic() + 18
    while True:
        view = query(mission)
        actual = view["granted"], view["instanceId"], view["revision"]
        if actual == expected and (reason is None or view["reason"] == reason):
            return view
        if time.monotonic() >= deadline:
            pytest.fail(f"{mission}: expected {expected}, last assessment {view!r}")
        time.sleep(.05)


def test_only_disjoint_route_is_granted(assessment):
    dispatch, query = assessment
    dispatch.publish()
    await_view(query, True, "first", 1)
    await_view(query, False, "first", 1, "cross", reason="requested route is occupied")
    for revision, attempt in ((2, None), (3, 1)):
        dispatch.publish(revision=revision, attempt=attempt)
        await_view(query, False, "first", revision, reason="unmatched dispatch mission")
    dispatch.publish(revision=4)
    await_view(query, True, "first", 4)


def test_inventory_error_and_dispatch_restart(assessment):
    dispatch, query = assessment
    dispatch.publish()
    await_view(query, True, "first", 1)
    with dispatch.lock:
        dispatch.reply = None
    await_view(query, False, "", 0)
    dispatch.publish(instance="second", revision=1, state=Inventory.LOADING)
    await_view(query, False, "second", 1)
    dispatch.publish(instance="second", revision=2)
    await_view(query, True, "second", 2)
    dispatch.publish(instance="third", revision=2, state=Inventory.CONFLICT)
    await_view(query, False, "third", 2)
    dispatch.publish(instance="fourth", revision=2)
    await_view(query, True, "fourth", 2)


def test_one_inventory_in_flight_for_concurrent_readers(assessment):
    dispatch, query = assessment
    entered, release = dispatch.publish(hold=True)
    assert entered.wait(12)
    try:
        with dispatch.lock:
            started = dispatch.calls
        with ThreadPoolExecutor(4) as readers:
            views = list(readers.map(query, ["free", "cross"] * 8))
        assert all(not view["granted"] for view in views)
        with dispatch.lock:
            assert dispatch.calls == started
            assert dispatch.peak == 1
    finally:
        release.set()
    await_view(query, True, "first", 1)


def test_delayed_reply_from_previous_stream_epoch_is_discarded(assessment):
    dispatch, query = assessment
    dispatch.publish()
    await_view(query, True, "first", 1)
    connected, send_snapshot = threading.Event(), threading.Event()
    with dispatch.lock:
        dispatch.next_stream = connected, send_snapshot
    assert connected.wait(8)

    old_entered, release_old = dispatch.publish(revision=2, hold=True)
    assert old_entered.wait(8)
    new_entered, release_new = dispatch.publish(instance="second", revision=2, hold=True, update_robots=False)
    try:
        send_snapshot.set()
        await_view(query, False, "", 0)
        release_old.set()
        assert new_entered.wait(8)
        # Even after the old RPC has completed, this new RPC is still waiting.
        # The old READY must not reappear in the route preview.
        view = query()
        assert (view["instanceId"], view["granted"]) == ("", False)
    finally:
        send_snapshot.set()
        release_old.set()
        with dispatch.lock:
            dispatch.robot_instance = "second"
        release_new.set()
    await_view(query, True, "second", 2)


@pytest.mark.parametrize("invalid_revision", [4, 5], ids=["rollback", "same_revision_changed"])
def test_rejects_inconsistent_revision(assessment, invalid_revision):
    dispatch, query = assessment
    dispatch.publish(revision=5)
    await_view(query, True, "first", 5)
    # Restricting A's own extension would still allow D on E–F. Only the invalid
    # version history makes this otherwise valid snapshot unusable.
    with dispatch.lock:
        before = dispatch.calls
    dispatch.publish(revision=invalid_revision, restricted=("A",))
    wait(lambda: dispatch.calls >= before + 2 and not query()["granted"],
         "repeated inconsistent snapshot is denied")
    dispatch.publish(revision=6, restricted=("A",))
    await_view(query, True, "first", 6)


def test_old_checkpoint_cannot_prove_a_new_inventory(assessment):
    dispatch, query = assessment
    dispatch.publish()
    await_view(query, True, "first", 1)
    # Reconnect telemetry first. Both dispatches report identical robot positions.
    # Await the second robot stream before holding a checkpoint from the first inventory.
    with dispatch.lock:
        dispatch.robot_instance = "second"
    wait(lambda: dispatch.robot_stream_instance == "second", "second robot snapshot")
    await_view(query, False, "first", 1, reason="control or dispatch inventory incomplete")
    old_entered, old_release, old_returned = threading.Event(), threading.Event(), threading.Event()
    new_entered, new_release = threading.Event(), threading.Event()
    with dispatch.lock:
        dispatch.checkpoint_hold = old_entered, old_release, old_returned
    try:
        assert old_entered.wait(10), "waiting for checkpoint refresh from the first dispatch"
        with dispatch.lock:
            dispatch.checkpoint_hold = new_entered, new_release, threading.Event()
        # Both dispatches report the same owners and revision. Only fresh matching
        # local command rows and a checkpoint from the new instance can authorize D.
        dispatch.publish(instance="second")
        await_view(query, False, "second", 1, reason="local command authority incomplete")
        old_release.set()
        assert old_returned.wait(2), "expected an old successful reply within the client deadline"
        assert new_entered.wait(10)
        view = query()
        assert (view["instanceId"], view["revision"], view["granted"]) == ("second", 1, False)
        assert view["reason"] == "local command authority incomplete"
    finally:
        old_release.set()
        new_release.set()
    await_view(query, True, "second", 1)


def test_ready_inventory_requires_current_robot_stream(assessment):
    dispatch, query = assessment
    dispatch.publish()
    await_view(query, True, "first", 1)
    # The old stream remains open, continually publishing the same live E position.
    dispatch.publish(instance="second", update_robots=False)
    await_view(query, False, "second", 1, reason="control or dispatch inventory incomplete")
    with dispatch.lock:
        assert dispatch.robot_stream_instance == "first"
        dispatch.robot_instance = "second"
    await_view(query, True, "second", 1)
