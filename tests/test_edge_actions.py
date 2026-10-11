"""Action'ы на рёбрах и на первом узле order'а (6.2), проверка области action'а при приёме order'а.

Нужен action "simulatedAction" с областью EDGE в factsheet (параметр duration — секунды):
без него тесты движения SKIPPED. Тесты отказов работают с любым роботом.

Правила, которые проверяются:
  - NONE на ребре выполняется на ходу; при съезде с ребра работающий → FINISHED,
    так и не начатый → FAILED;
  - SOFT/HARD на ребре выполняется стоя на узле перед ребром, потом робот едет;
  - action'ы первого узла и первого ребра order'а, а также ребра, открытого update'ом,
    тоже выполняются (on_node_reached для них не вызывается).
"""

from __future__ import annotations

import pytest

from fms_client import is_idle
from order_builder import Route, build_order, new_order_id
from action_helpers import (
    action, send_instant, has_error_for, wait_instant, pause, unpause, wait_stopped,
    ensure_unpaused, order_action_status, wait_order_action, assert_holds, describe,
)

SIM = "simulatedAction"


@pytest.fixture(autouse=True)
def release_pause(helper):
    ensure_unpaused(helper)
    yield
    ensure_unpaused(helper)


@pytest.fixture
def sim_action(fms):
    """Робот объявляет simulatedAction на рёбрах и узлах — иначе тест пропускается."""
    fs = fms.latest("factsheet") or fms.wait_message("factsheet", lambda m: True, 3.0)
    declared = {a["actionType"]: a for a in fs["protocolFeatures"]["mobileRobotActions"]}
    scopes = set(declared.get(SIM, {}).get("actionScopes", []))
    if not {"EDGE", "NODE"} <= scopes:
        pytest.skip(f"robot does not declare {SIM} with EDGE and NODE scope in factsheet")


def sim(duration: float, blocking: str = "NONE") -> dict:
    return action(SIM, {"duration": duration}, blocking=blocking)


def route_order(robot, count: int, step: float, edges: dict | None = None, nodes: dict | None = None,
                released: int | None = None):
    """Order по прямой: edges / nodes — {индекс: [action, ...]}."""
    route = Route(robot.pose(), count=count, step=step)
    order = build_order(new_order_id(), 0, route.nodes, released=released or count + 1,
                        map_id=route.map_id)
    for i, acts in (edges or {}).items():
        order["edges"][i]["actions"] = acts
    for i, acts in (nodes or {}).items():
        order["nodes"][i]["actions"] = acts
    return route, order


def send_accepted(robot, order) -> int:
    mark = robot.send(order)
    robot.wait_accepted(order, mark)
    return mark


def wait_running_while_driving(robot, a: dict, since: int, timeout: float) -> dict:
    return robot.fms.wait_state(
        lambda s: order_action_status(s, a["actionId"]) == "RUNNING" and s["driving"],
        timeout, since=since, what=f"{a['actionType']} ({a['actionId']}) RUNNING while driving")


def first_state(robot, since: int, predicate) -> dict | None:
    return next((s for s in robot.fms.states(since) if predicate(s)), None)


# ------------------------------------------------------------------ NONE на ребре

def test_none_action_on_first_edge_runs_while_driving(robot, sim_action):
    a = sim(60.0)                                  # дольше, чем ехать по ребру
    route, order = route_order(robot, count=2, step=1.5, edges={0: [a]})
    mark = send_accepted(robot, order)

    wait_running_while_driving(robot, a, mark, robot.drive_timeout(1))
    s = robot.wait_last_node(route.node_id(1), robot.drive_timeout(2), since=mark)
    # Съехали с ребра: работающий action ребра завершён.
    assert order_action_status(s, a["actionId"]) == "FINISHED", describe(s)

    s = robot.wait_idle(robot.drive_timeout(2))
    assert s["lastNodeId"] == route.node_id(2)
    stopped = first_state(robot, mark, lambda x: x["lastNodeId"] == route.node_id(1) and not x["driving"])
    assert stopped is None, f"NONE edge action must not stop the robot at n1\n  {describe(stopped)}"


def test_short_edge_action_finishes_on_its_own(robot, sim_action):
    a = sim(0.5)
    route, order = route_order(robot, count=1, step=3.0, edges={0: [a]})
    mark = send_accepted(robot, order)

    s = wait_order_action(robot, a, "FINISHED", mark, robot.drive_timeout(1))
    assert s["lastNodeId"] == route.node_id(0), "short action must finish while still on the edge"
    robot.wait_idle(robot.drive_timeout(3))


@pytest.mark.parametrize("when", ["at_decision_point", "while_driving"])
def test_action_on_edge_released_by_update(robot, sim_action, when):
    route, order = route_order(robot, count=2, step=1.5, released=2)    # n2 — horizon
    mark = send_accepted(robot, order)
    if when == "at_decision_point":
        robot.fms.wait_state(lambda s: s["lastNodeId"] == route.node_id(1) and not s["driving"]
                             and s.get("newBaseRequest"), robot.drive_timeout(1), since=mark,
                             what="robot waits at decision point")
    else:
        robot.fms.wait_state(lambda s: s["driving"], robot.t(5.0), since=mark, what="driving")

    a = sim(60.0)
    update = build_order(order["orderId"], 1, route.nodes[1:], released=2, map_id=route.map_id, first_seq=2)
    update["edges"][0]["actions"] = [a]
    mark = send_accepted(robot, update)

    wait_running_while_driving(robot, a, mark, robot.drive_timeout(2))
    s = robot.wait_idle(robot.drive_timeout(3))
    assert s["lastNodeId"] == route.node_id(2)
    assert order_action_status(s, a["actionId"]) == "FINISHED", describe(s)


def test_edge_action_fails_if_edge_is_never_driven(robot, sim_action):
    a = sim(60.0)
    route, order = route_order(robot, count=2, step=2.0, edges={1: [a]})
    mark = send_accepted(robot, order)
    robot.fms.wait_state(lambda s: s["driving"], robot.t(5.0), since=mark, what="driving")

    c = action("cancelOrder")
    mark = send_instant(robot, c)
    s = wait_instant(robot, c, "FINISHED", mark)
    assert order_action_status(s, a["actionId"]) == "FAILED", describe(s)
    assert is_idle(s)


# ------------------------------------------------------------- SOFT/HARD на ребре

@pytest.mark.parametrize("edge_index", [0, 1], ids=["first_edge", "second_edge"])
@pytest.mark.parametrize("blocking", ["HARD", "SOFT"])
def test_blocking_edge_action_runs_standing_before_edge(robot, sim_action, edge_index, blocking):
    a = sim(2.0, blocking)
    route, order = route_order(robot, count=2, step=1.5, edges={edge_index: [a]})
    mark = send_accepted(robot, order)

    wait_order_action(robot, a, "RUNNING", mark, robot.drive_timeout(edge_index * 1.5))
    s = wait_order_action(robot, a, "FINISHED", mark, robot.t(2.0 * 1.2 + 3.0))
    assert s["lastNodeId"] == route.node_id(edge_index), \
        f"{blocking} edge action must run at the node before the edge\n  {describe(s)}"

    for x in robot.fms.states(mark):
        assert not (order_action_status(x, a["actionId"]) == "RUNNING" and x["driving"]), \
            f"{blocking} edge action is RUNNING while robot drives\n  {describe(x)}"

    s = robot.wait_idle(robot.drive_timeout(3))
    assert s["lastNodeId"] == route.node_id(2)
    assert order_action_status(s, a["actionId"]) == "FINISHED", "leaving the edge must not change a finished action"


# ------------------------------------------------------------------ пауза и отмена

def test_pause_during_edge_action(robot, sim_action):
    a = sim(60.0)
    route, order = route_order(robot, count=1, step=4.0, edges={0: [a]})
    mark = send_accepted(robot, order)
    wait_running_while_driving(robot, a, mark, robot.drive_timeout(1))

    mark = robot.fms.mark()
    pause(robot)
    wait_order_action(robot, a, "PAUSED", mark, robot.t(3.0))
    wait_stopped(robot, mark)
    assert_holds(robot, 2.0, lambda s: order_action_status(s, a["actionId"]) == "PAUSED" and not s["driving"],
                 "edge action must stay PAUSED and robot must stand while paused")

    mark = robot.fms.mark()
    unpause(robot)
    wait_running_while_driving(robot, a, mark, robot.t(3.0))
    s = robot.wait_idle(robot.drive_timeout(4))
    assert s["lastNodeId"] == route.node_id(1)
    assert order_action_status(s, a["actionId"]) == "FINISHED"


def test_cancel_during_edge_action(robot, sim_action):
    a = sim(60.0)                                  # cancelAllowed: отменяется сразу
    route, order = route_order(robot, count=1, step=4.0, edges={0: [a]})
    mark = send_accepted(robot, order)
    wait_running_while_driving(robot, a, mark, robot.drive_timeout(1))

    c = action("cancelOrder")
    mark = send_instant(robot, c)
    s = wait_instant(robot, c, "FINISHED", mark)
    assert order_action_status(s, a["actionId"]) == "FAILED", describe(s)
    wait_stopped(robot, mark)
    assert_holds(robot, 1.5, lambda x: not x["driving"], "robot must not drive after cancelOrder")


# ------------------------------------------------------------------ первый узел order'а

def test_blocking_action_on_first_node_runs_before_driving(robot, sim_action):
    a = sim(2.0, "HARD")
    route, order = route_order(robot, count=1, step=1.5, nodes={0: [a]})
    mark = send_accepted(robot, order)

    s = wait_order_action(robot, a, "FINISHED", mark, robot.t(2.0 * 1.2 + 5.0))
    moved_before = first_state(robot, mark, lambda x: x["driving"]
                               and order_action_status(x, a["actionId"]) != "FINISHED")
    assert moved_before is None, f"robot drove before the HARD action on its start node finished\n  {describe(moved_before)}"
    s = robot.wait_idle(robot.drive_timeout(2))
    assert s["lastNodeId"] == route.node_id(1)


def test_none_action_on_first_node_does_not_hold_robot(robot, sim_action):
    a = sim(3.0)
    route, order = route_order(robot, count=1, step=3.0, nodes={0: [a]})
    mark = send_accepted(robot, order)

    wait_running_while_driving(robot, a, mark, robot.t(5.0))
    s = robot.wait_idle(robot.drive_timeout(3) + 3.0)
    assert s["lastNodeId"] == route.node_id(1)
    assert order_action_status(s, a["actionId"]) == "FINISHED"


# ------------------------------------------------------------------ область action'а

@pytest.mark.parametrize("where, action_type", [
    ("edge", "pick"),          # pick/drop — только на узле
    ("node", "cancelOrder"),   # только instant
    ("edge", "startPause"),    # только instant
    ("node", "flyToTheMoon"),  # неизвестный тип
])
def test_action_in_wrong_scope_is_rejected(robot, where, action_type):
    a = action(action_type, blocking="HARD" if action_type == "pick" else "NONE")
    route, order = route_order(robot, count=1, step=1.0,
                               edges={0: [a]} if where == "edge" else None,
                               nodes={1: [a]} if where == "node" else None)
    mark = robot.send(order)
    s = robot.fms.wait_state(lambda x: has_error_for(x, "INVALID_ORDER_ACTION", a["actionId"]),
                             robot.t(3.0), since=mark,
                             what=f"INVALID_ORDER_ACTION for {action_type} on {where}")
    assert s["orderId"] != order["orderId"], "rejected order must not become the current order"
    assert_holds(robot, 1.0, lambda x: x["orderId"] != order["orderId"] and not x["driving"],
                 "rejected order must not be executed")
