"""Приём и выполнение order'ов без action'ов (6.1, 6.6.2)."""

import math

from fms_client import has_error
from order_builder import Route, build_order, new_order_id


def node_ids(nodes):
    return [n["nodeId"] for n in nodes]


def visited_nodes(fms, since, order_id):
    """lastNodeId по порядку (без повторов) в state этого order'а."""
    seen = []
    for s in fms.states(since):
        if s["orderId"] == order_id and s["lastNodeId"] and (not seen or seen[-1] != s["lastNodeId"]):
            seen.append(s["lastNodeId"])
    return seen


def test_order_without_horizon_is_driven_to_the_end(robot, fms):
    route = Route(robot.pose(), count=3)
    order = build_order(new_order_id(), 0, route.nodes, released=4, map_id=route.map_id)
    mark = robot.send(order)

    s = robot.wait_accepted(order, mark)
    # Первый узел робот считает пройденным: в nodeStates его нет (6.1.1).
    assert node_ids(s["nodeStates"]) == [route.node_id(i) for i in (1, 2, 3)]
    assert len(s["edgeStates"]) == 3
    assert all(n["released"] for n in s["nodeStates"] + s["edgeStates"])

    s = robot.wait_idle(robot.drive_timeout(3))
    assert s["lastNodeId"] == route.node_id(3)
    assert s["lastNodeSequenceId"] == 6
    # orderId и orderUpdateId остаются до следующего order'а (6.6.7).
    assert (s["orderId"], s["orderUpdateId"]) == (order["orderId"], 0)

    visited = visited_nodes(fms, mark, order["orderId"])
    expected = [route.node_id(i) for i in range(4)]
    assert visited == [n for n in expected if n in visited], f"nodes passed out of order: {visited}"
    assert visited[-1] == route.node_id(3)

    pos = s["mobileRobotPosition"]
    end = route.nodes[3]
    assert math.hypot(pos["x"] - end.x, pos["y"] - end.y) <= 0.5, "robot stopped away from the last node"


def test_robot_drives_when_order_received(robot):
    route = Route(robot.pose(), count=2)
    order = build_order(new_order_id(), 0, route.nodes, released=3, map_id=route.map_id)
    mark = robot.send(order)
    robot.fms.wait_state(lambda s: s["driving"], robot.t(5.0), since=mark, what="driving == true")
    robot.wait_idle(robot.drive_timeout(2))


def test_resent_identical_order_is_ignored(robot):
    """Повтор того же order'а (fms не дождался state) — молча игнорируется (6.1.4.5)."""
    route = Route(robot.pose(), count=2)
    order = build_order(new_order_id(), 0, route.nodes, released=3, map_id=route.map_id)
    mark = robot.send(order)
    robot.wait_accepted(order, mark)

    robot.fms.send_order(order)  # новый headerId и timestamp, содержимое то же
    robot.assert_stays(lambda s: not has_error(s, "SAME_ORDER_UPDATE_ID"), 1.0,
                       "identical resend must not raise SAME_ORDER_UPDATE_ID")
    s = robot.wait_idle(robot.drive_timeout(2))
    assert s["lastNodeId"] == route.node_id(2)


def test_update_while_driving_extends_route_without_stopping(robot, fms):
    route = Route(robot.pose(), count=4)
    oid = new_order_id()
    order = build_order(oid, 0, route.nodes[:3], released=3, map_id=route.map_id)  # base до n2
    mark = robot.send(order)
    robot.wait_accepted(order, mark)
    robot.wait_last_node(route.node_id(1), robot.drive_timeout(1), since=mark)

    # Робот ещё едет к n2 — продлеваем base до n4. Stitching node — n2 (sequenceId 4).
    update = build_order(oid, 1, route.nodes[2:], released=3, map_id=route.map_id, first_seq=4)
    robot.fms.send_order(update)

    s = robot.wait_idle(robot.drive_timeout(4))
    assert s["lastNodeId"] == route.node_id(4)
    assert s["orderUpdateId"] == 1
    stopped_at_old_end = [x for x in fms.states(mark)
                          if x["lastNodeId"] == route.node_id(2) and not x["driving"]]
    assert not stopped_at_old_end, "robot stopped at the old decision point although the base was extended"


def test_update_to_completed_order(robot):
    """Fms может продолжить уже выполненный order update'ом (рисунок 8, шаг 10)."""
    route = Route(robot.pose(), count=4)
    oid = new_order_id()
    order = build_order(oid, 0, route.nodes[:3], released=3, map_id=route.map_id)
    mark = robot.send(order)
    robot.wait_accepted(order, mark)
    robot.wait_idle(robot.drive_timeout(2))

    update = build_order(oid, 1, route.nodes[2:], released=3, map_id=route.map_id, first_seq=4)
    mark = robot.send(update)
    robot.wait_accepted(update, mark)
    s = robot.wait_idle(robot.drive_timeout(2))
    assert s["lastNodeId"] == route.node_id(4)


def test_single_node_order_completes_immediately(robot):
    """Order из одного узла там, где робот стоит: принят и сразу выполнен."""
    route = Route(robot.pose(), count=0)
    order = build_order(new_order_id(), 0, route.nodes, released=1, map_id=route.map_id)
    mark = robot.send(order)
    s = robot.wait_accepted(order, mark)
    assert s["nodeStates"] == [] and s["edgeStates"] == []
    robot.wait_idle(robot.t(2.0))
