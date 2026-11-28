"""Two physical simulator sessions. Commands are issued only through the real fleet_control HTTP API."""
import json
import os
from contextlib import ExitStack

import grpc
import pytest

import fleet_database_pb2 as db
import fleet_dispatch_pb2 as dp
import fms_types_pb2 as fms
from test_control_checkpoint_stand import Stand
from test_dispatch_startup import first
from test_move_restart_stand import stop, wait


pytestmark = pytest.mark.skipif(os.getenv("INDEPENDENT_MOTION_STAND") != "1",
                                reason="isolated two-robot WSL stand")
SERIAL = {"A": "amr101", "D": "amr102"}
BASES = {"A": "lead", "D": "independent"}


class MotionStand(Stand):
    def control(self):
        # The parent stand's fault proxy is specific to one mission. Connect
        # both real services directly for this two-mission scenario.
        self.ctl = self.launch("control", [self.build / "fleet_control/fleet_control"],
                               DATABASE_ADDRESS=self.dbaddr, DISPATCH_ADDRESS=self.fdaddr,
                               HTTP_ADDRESS=self.ctladdr)
        wait(lambda: self.http("GET", "/api/health")[0] == 200, "control HTTP", 25)

    def prepare(self, crossing):
        import yaml

        # D has a separate process so A can stop replying without stopping D.
        cfg = yaml.safe_load((self.tmp / "simulator.yaml").read_text())
        cfg["mqtt"]["client_id"] = "independent-102"
        cfg["robots"][0]["serial_number"] = "102"
        cfg["initial_state"].update(x=2., y=6.5)
        path = self.tmp / "simulator-D.yaml"
        path.write_text(yaml.safe_dump(cfg))
        target = "/home/ubuntu/fleet-manager/robot_simulation/config/config.yaml"
        self.launch("simulator-D", ["proot", "-b", f"{path}:{target}",
                    self.build / "robot_simulation/robot_simulation"], PROOT_NO_SECCOMP="1")

        # One MQTT connection preserves the observer's event order across both robots.
        # Count fresh publishes, including identical application-level resends, while
        # ignoring retransmission of the very same QoS packet to this subscriber.
        self.wire.client.on_message = lambda c, u, m: None if m.dup else self.wire.message(c, u, m)
        self.wire.sub.clear()
        rc, _ = self.wire.client.subscribe("amr/v3/ktc/amr102/#", qos=1)
        assert rc == 0 and self.wire.sub.wait(5)

        # At 1 m/s leave half a minute after M to observe progress during a cold restart.
        end = 13 if crossing else 60
        points = {"B": (1, 2.5), "C": (end, 2.5), "G": (end + 2, 2.5),
                  "E": (2, 6.5), "M": (12, 6.5), "F": (42, 6.5)}
        edges = ["BC", "CB", "CG", "GC", "EM", "ME", "MF", "FM", "EC", "CE"]
        layout = dict(id="map_1", name="Independent trips", width=1300, height=180,
                      meta={"resolution": .05, "origin": [0, 0, 0]},
                      waypoints=[dict(id=k, u=round(x / .05), v=180-round(y / .05))
                                 for k, (x, y) in points.items()],
                      edges=[{"id": e, "from": e[0], "to": e[1]} for e in edges],
                      stations=[], zones=[], assignedRobots=[])

        def create_map():
            code, result = self.http("POST", "/api/fms/maps", layout)
            if code == 503:
                return False
            assert code == 201, (code, result)
            return True

        wait(create_map, "map saved")
        for robot, node in (("A", "B"), ("D", "E")):
            code, body = self.http("POST", "/api/fms/robots",
                                   dict(id=robot, manufacturer="ktc", serialNumber=SERIAL[robot]))
            assert code == 201, (code, body)
            wait(lambda: (r := self.status(robot)) and r.connection == fms.CONNECTION_STATE_ONLINE,
                 f"{robot} online", 25)
            code, body = self.http("POST", f"/api/fms/robots/{robot}/position", dict(mapId="map_1", nodeId=node))
            assert code == 202, (code, body)
            wait(lambda: (r := self.status(robot)) and r.HasField("observed_order") and
                 r.observed_order.idle and r.last_node_id == node, f"{robot} on {node}", 25)
        self.mark = self.wire.mark()

    def status(self, robot):
        robots = first(self.api.StreamRobotStatus, dp.StreamRobotStatusRequest(robot_ids=[robot])).snapshot.robots
        return next((r for r in robots if r.robot_id == robot), None)

    def trip(self, mid, robot, goal):
        code, body = self.http("POST", "/api/fms/missions", dict(
            id=mid, name=mid, requestedRobotId=robot, mapId="map_1", steps=[{"nodeId": goal}]))
        assert code == 201, (code, body)

    def state(self, robot, mid, predicate, description, since=None):
        return self.wire.state(self.mark if since is None else since, lambda s:
                               s["serialNumber"] == SERIAL[robot] and s["orderId"] == mid+"-s0" and
                               predicate(s), description, timeout=70)

    def arrived(self, robot, mid, node):
        return self.state(robot, mid, lambda s: s["orderUpdateId"] == 1 and s["lastNodeId"] == node
                          and not s["driving"] and not s["nodeStates"] and not s["edgeStates"],
                          f"{robot} physically stopped at {node}")

    def remaining(self, mid, source):
        status, body = self.http("GET", "/api/fms/routes?mapId=map_1")
        if status == 200:
            return next((r["released"]["nodes"] for r in body["items"]
                         if r["missionId"] == mid and r["progressSource"] == source), None)

    def inventory(self):
        return self.api.GetRecoveryInventory(dp.GetRecoveryInventoryRequest(), timeout=2)

    def sealed(self, mid):
        doc = self.storage.GetMission(db.GetMissionRequest(mission_id=mid), timeout=3).mission
        assert doc.state not in {"FAILED", "CANCELED"}, doc
        return doc.state == "COMPLETED" and doc.dispatch_final_version

    def orders(self, robot, mid):
        orders = []
        for raw in self.wire.raw("order", self.mark):
            obj = json.loads(raw)
            if obj["serialNumber"] == SERIAL[robot] and obj["orderId"] == mid+"-s0":
                orders.append(raw)
        return orders

    def check_grants(self, robot, mid, path):
        rows = self.sql.execute("SELECT execution FROM dispatch_execution_orders WHERE mission_id=%s "
                                "ORDER BY update_id", (mid,)).fetchall()
        durable = [db.MissionExecution.FromString(bytes(row[0])).order.sent_order_json.encode() for row in rows]
        orders = self.orders(robot, mid)
        assert orders == durable, f"{robot}/{mid}: missing, changed or repeated publish"
        parsed = [json.loads(raw) for raw in orders]
        assert [p["orderUpdateId"] for p in parsed] == [0, 1]
        released = [[n["nodeId"] for n in p["nodes"] if n["released"]] for p in parsed]
        assert released == [path[:1], path]
        commands = self.storage.ListControlCommands(db.ListControlCommandsRequest(
            mission_id=mid, include_resolved=True), timeout=3).commands
        assert sorted(c.command.kind for c in commands) == sorted([
            db.CONTROL_COMMAND_KIND_ADD_MISSION, db.CONTROL_COMMAND_KIND_EXTEND_MISSION])

    def broker_barrier(self):
        # A scenario can place several barriers without resetting its MQTT history.
        mark = self.wire.mark()
        self.wire.client.publish("amr/v3/ktc/amr101/barrier", b"checked", qos=1).wait_for_publish(3)
        wait(lambda: self.wire.raw("barrier", mark) == [b"checked"], "observer caught up")

    def unresolved_a(self):
        def inventory_with_a():
            view = self.inventory()
            if (view.state == dp.GetRecoveryInventoryResponse.READY and "A" in view.live_restrictions.robots
                    and any(m.mission_id == BASES["A"] and m.robot_id == "A" for m in view.missions)):
                return view
        view = wait(inventory_with_a, "current inventory with unresolved A", 18)
        robot = self.status("A")
        assert robot is not None and not robot.HasField("observed_order")
        with pytest.raises(grpc.RpcError) as error:
            first(self.api.StreamMissionStatus, dp.StreamMissionStatusRequest())
        assert error.value.code() == grpc.StatusCode.UNAVAILABLE
        return view


@pytest.mark.parametrize("mode", ["normal", "restart", "crossing"])
def test_independent_motion(mode, tmp_path):
    with ExitStack() as stack:
        s = MotionStand(stack, tmp_path)
        s.prepare(crossing=mode == "crossing")
        lead, trip = BASES["A"], BASES["D"]
        s.trip(lead, "A", "C")
        moving_a = lambda state: (state["orderUpdateId"] == 1 and state["driving"] and
                                  state["lastNodeId"] == "B" and state["mobileRobotPosition"]["x"] > 1.4)
        before = s.state("A", lead, moving_a, "A is between B and C")

        if mode == "crossing":
            check_crossing(s, lead, trip)
            return

        s.trip(trip, "D", "F")
        s.state("D", trip, lambda x: x["orderUpdateId"] == 1 and x["driving"] and
                x["lastNodeId"] == "E", "D accepted E-M-F")
        wait(lambda: s.remaining(trip, "mission_stream") == ["E", "M", "F"],
             "control sees full issued route through the ordinary stream")

        if mode == "restart":
            old = s.inventory().instance_id
            s.frozen_sim(True)
            # Resume before the stand tears down subprocess groups, also on assertion failure.
            stack.callback(s.frozen_sim, False)
            stop(s.fd, abrupt=True)
            s.dispatch()
            view = s.unresolved_a()
            assert old and view.instance_id != old
            row, = [m for m in view.missions if m.mission_id == trip and m.robot_id == "D"]
            assert row.HasField("add_attempt")
            req = dp.GetMissionRequest(mission_id=trip, expected_instance_id=view.instance_id,
                                       expected_robot_id="D", expected_add_attempt=row.add_attempt)
            # Retry only until D has provided fresh evidence after reconnect.
            def scoped_read():
                try:
                    reply = s.api.GetMission(req, timeout=2)
                except grpc.RpcError as e:
                    if e.code() in (grpc.StatusCode.UNAVAILABLE, grpc.StatusCode.FAILED_PRECONDITION):
                        return False
                    raise
                assert reply.instance_id == view.instance_id and reply.HasField("add_attempt")
                assert reply.add_attempt == row.add_attempt and reply.status.robot_id == "D"
                assert reply.mission.mission_id == reply.status.mission_id == trip
                assert reply.status.state == fms.MISSION_STATE_RUNNING
                return True
            wait(scoped_read, "addressed GetMission for D while A is unresolved", 18)

        source = "scoped_read" if mode == "restart" else "mission_stream"
        wait(lambda: s.remaining(trip, source) == ["M", "F"], f"control observed D passing M via {source}", 40)
        s.arrived("D", trip, "F")
        if mode == "normal":
            after = s.state("A", lead, moving_a, "A is still moving after D reached F", s.wire.mark())
            assert after["mobileRobotPosition"]["x"] > before["mobileRobotPosition"]["x"]
            s.arrived("A", lead, "C")
        else:
            s.unresolved_a()

        wait(lambda: s.sealed(trip), "control recorded D's final", 35)
        s.broker_barrier()
        s.check_grants("A", lead, ["B", "C"])
        s.check_grants("D", trip, ["E", "M", "F"])


def check_crossing(s, lead, trip):
    s.trip(trip, "D", "C")
    waiting = lambda x: (x["orderUpdateId"] == 0 and x["lastNodeId"] == "E" and not x["driving"]
                         and x["nodeStates"] and all(not n["released"] for n in x["nodeStates"]))
    s.state("D", trip, waiting, "D waits at E with C in horizon")
    initial, = map(json.loads, s.orders("D", trip))
    assert [n["nodeId"] for n in initial["nodes"] if n["released"]] == ["E"]
    s.arrived("A", lead, "C")
    wait(lambda: s.sealed(lead), "A completed at C")
    s.state("D", trip, waiting, "parked A still holds C", s.wire.mark())
    assert len(s.orders("D", trip)) == 1

    clear = "clear-c"
    s.trip(clear, "A", "G")
    s.arrived("A", clear, "G")
    s.arrived("D", trip, "C")
    wait(lambda: s.sealed(clear) and s.sealed(trip), "both final records", 35)
    s.broker_barrier()
    s.check_grants("A", lead, ["B", "C"])
    s.check_grants("A", clear, ["C", "G"])
    s.check_grants("D", trip, ["E", "C"])

    # Both topics are observed on the same MQTT subscription connection.
    # C becomes available after A is at G; an early grant fails even if both ultimately finish.
    with s.wire.lock:
        events = [(kind, json.loads(raw)) for kind, raw in s.wire.events[s.mark:] if kind in ("order", "state")]
    clear_pos = next(i for i, (k, msg) in enumerate(events) if k == "state" and
                     msg["serialNumber"] == SERIAL["A"] and msg["orderId"] == clear+"-s0" and
                     msg["lastNodeId"] == "G")
    cross_grant = next(i for i, (k, msg) in enumerate(events) if k == "order" and
                       msg["serialNumber"] == SERIAL["D"] and msg["orderId"] == trip+"-s0" and
                       any(n["released"] and n["nodeId"] == "C" for n in msg["nodes"]))
    assert cross_grant > clear_pos
