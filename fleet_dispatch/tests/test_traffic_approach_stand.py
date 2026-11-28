"""Drive to a safe waiting node, then resume the same order on a private WSL stand."""

import json
import os
from contextlib import ExitStack

import grpc
import pytest

import fleet_database_pb2 as db
import fleet_dispatch_pb2 as dp
from test_control_checkpoint_stand import Proxy
from test_independent_motion_stand import MotionStand, SERIAL
from test_move_restart_stand import stop, wait


pytestmark = pytest.mark.skipif(os.getenv("TRAFFIC_APPROACH_STAND") != "1",
                                reason="private WSL traffic approach stand")
PARK = "approach-park"
APPROACH = "approach-trip"
CLEAR = "approach-clear"


class ApproachProxy(Proxy):
    lose_partial_reply = False

    def service(self, details):
        method = details.method
        name = method.rsplit("/", 1)[-1]
        if name not in {"ExtendMission", "GetExecutionCheckpoint"}:
            return super().service(details)

        def unary(raw, context):
            try:
                reply = self.channel.unary_unary(method)(raw, timeout=3)
            except grpc.RpcError as error:
                context.abort(error.code(), error.details())
            if name == "ExtendMission":
                request = dp.ExtendMissionRequest.FromString(raw)
                if self.lose_partial_reply and request.mission_id == APPROACH and request.revision == 1:
                    self.lose_partial_reply = False
                    self.hit("lost partial reply")
                    context.abort(grpc.StatusCode.UNAVAILABLE, "partial extension accepted; reply lost")
            return reply

        return grpc.unary_unary_rpc_method_handler(unary)


class ApproachStand(MotionStand):
    def control(self):
        if not isinstance(self.fdproxy, ApproachProxy):
            self.fdproxy = ApproachProxy(self.stack, self.fdaddr)
        self.epoch["control"] += 1
        self.ctl = self.launch(f"control-{self.epoch['control']}",
                               [self.build / "fleet_control/fleet_control"],
                               DATABASE_ADDRESS=self.dbaddr,
                               DISPATCH_ADDRESS=f"127.0.0.1:{self.fdproxy.port}", HTTP_ADDRESS=self.ctladdr)
        wait(lambda: self.http("GET", "/api/health")[0] == 200, "control HTTP", 25)

    def commands_for(self, mid):
        return self.storage.ListControlCommands(
            db.ListControlCommandsRequest(mission_id=mid, include_resolved=True), timeout=3).commands

    def closed(self, mid):
        rows = self.commands_for(mid)
        return bool(rows) and all(row.resolved for row in rows)


@pytest.mark.parametrize("restart", [False, True], ids=["normal", "lost-reply-and-restart"])
def test_traffic_approach(restart, tmp_path):
    with ExitStack() as stack:
        s = ApproachStand(stack, tmp_path)
        s.prepare(crossing=True)

        # In the existing stand layout the route B–C–E–M shares M with D.
        # D will later clear M by driving to F, independently of waiting at E.
        s.trip(PARK, "D", "M")
        s.arrived("D", PARK, "M")
        wait(lambda: s.sealed(PARK) and s.closed(PARK), "D parked at M and journal closed", 35)

        s.fdproxy.lose_partial_reply = restart
        s.trip(APPROACH, "A", "M")

        def at_boundary(state):
            return (state["orderUpdateId"] == 1 and state["lastNodeId"] == "E" and
                    not state["driving"] and state["nodeStates"] and
                    all(not node["released"] for node in state["nodeStates"]))

        s.state("A", APPROACH, at_boundary, "A has approached E while D still occupies M")
        assert s.status("D").last_node_id == "M"
        s.broker_barrier()
        prefix_orders = s.orders("A", APPROACH)
        assert [json.loads(raw)["orderUpdateId"] for raw in prefix_orders] == [0, 1]
        partial = json.loads(prefix_orders[1])
        assert [n["nodeId"] for n in partial["nodes"] if n["released"]] == ["B", "C", "E"]
        assert [n["nodeId"] for n in partial["nodes"] if not n["released"]] == ["M"]

        if restart:
            assert s.fdproxy.count("lost partial reply") == 1
            stop(s.ctl, abrupt=True)
            s.control()
            wait(lambda: s.remaining(APPROACH, "mission_stream") == ["E"],
                 "control restored only the acknowledged base", 30)
            s.broker_barrier()
            assert s.orders("A", APPROACH) == prefix_orders

        s.trip(CLEAR, "D", "F")
        s.arrived("D", CLEAR, "F")
        s.state("A", APPROACH, lambda state:
                state["orderUpdateId"] == 2 and state["lastNodeId"] == "M" and
                not state["driving"] and not state["nodeStates"] and not state["edgeStates"],
                "A reached M on the same order")
        wait(lambda: s.sealed(APPROACH) and s.closed(APPROACH), "A completed and journal closed", 35)
        s.broker_barrier()

        orders = s.orders("A", APPROACH)
        assert [json.loads(raw)["orderUpdateId"] for raw in orders] == [0, 1, 2]
        assert orders[:2] == prefix_orders
        assert [n["nodeId"] for n in json.loads(orders[2])["nodes"] if n["released"]] == ["E", "M"]

        commands = s.commands_for(APPROACH)
        assert sum(row.command.kind == db.CONTROL_COMMAND_KIND_ADD_MISSION for row in commands) == 1
        extensions = sorted((row.command for row in commands
                             if row.command.kind == db.CONTROL_COMMAND_KIND_EXTEND_MISSION),
                            key=lambda command: command.revision)
        assert [(command.revision, dp.ExtendMissionRequest.FromString(command.request_proto).released_nodes)
                for command in extensions] == [(1, 3), (2, 4)]

        # The broker observer sees both robots on one connection. A may enter
        # M only after D has reported reaching the next node F.
        with s.wire.lock:
            events = [(kind, json.loads(raw)) for kind, raw in s.wire.events[s.mark:]
                      if kind in ("order", "state")]
        cleared = next(i for i, (kind, msg) in enumerate(events) if kind == "state" and
                       msg["serialNumber"] == SERIAL["D"] and msg["orderId"] == CLEAR + "-s0" and
                       msg["lastNodeId"] == "F")
        granted = next(i for i, (kind, msg) in enumerate(events) if kind == "order" and
                       msg["serialNumber"] == SERIAL["A"] and msg["orderId"] == APPROACH + "-s0" and
                       msg["orderUpdateId"] == 2)
        assert cleared < granted
