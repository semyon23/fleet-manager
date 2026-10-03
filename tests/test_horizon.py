"""Base и horizon (6.1.2): остановка на decision point, ожидание update, замена horizon'а."""

import math

from fms_client import is_idle
from order_builder import Route, build_order, new_order_id


def test_robot_waits_at_decision_point_until_update(robot):
    route = Route(robot.pose(), count=4)
    oid = new_order_id()
    # base n0..n2, horizon n3, n4
    order = build_order(oid, 0, route.nodes, released=3, map_id=route.map_id)
    mark = robot.send(order)
    s = robot.wait_accepted(order, mark)
    assert [n["released"] for n in s["nodeStates"]] == [True, True, False, False]

    robot.wait_last_node(route.node_id(2), robot.drive_timeout(2), since=mark)
    s = robot.fms.wait_state(lambda x: not x["driving"], robot.t(5.0), since=robot.since_latest(),
                             what="stopped at decision point")
    assert s["lastNodeId"] == route.node_id(2)
    # Horizon остался в nodeStates — order не завершён, робот не idle.
    assert [(n["nodeId"], n["released"]) for n in s["nodeStates"]] == \
           [(route.node_id(3), False), (route.node_id(4), False)]
    assert not is_idle(s)
    robot.fms.wait_state(lambda x: x.get("newBaseRequest") is True, robot.t(3.0),
                         since=robot.since_latest(), what="newBaseRequest == true at decision point")

    # По horizon'у без update'а не едет.
    robot.assert_stays(lambda x: x["lastNodeId"] == route.node_id(2) and not x["driving"], 2.0,
                       "robot must not drive into the horizon")

    update = build_order(oid, 1, route.nodes[2:], released=3, map_id=route.map_id, first_seq=4)
    mark = robot.send(update)
    robot.wait_accepted(update, mark)
    s = robot.wait_idle(robot.drive_timeout(2))
    assert s["lastNodeId"] == route.node_id(4)
    assert not s.get("newBaseRequest"), "newBaseRequest must be reset after the base was extended"


def test_update_replaces_horizon(robot, fms):
    """Update может заменить horizon другим маршрутом (6.1.2)."""
    start = robot.pose()
    route = Route(start, count=2)
    oid = new_order_id()
    left = route.branch(2, count=2, step=1.0, heading=start.theta + math.pi / 2, tag="L")
    right = route.branch(2, count=2, step=1.0, heading=start.theta - math.pi / 2, tag="R")

    order = build_order(oid, 0, route.nodes + left, released=3, map_id=route.map_id)  # horizon — налево
    mark = robot.send(order)
    robot.wait_accepted(order, mark)
    robot.wait_last_node(route.node_id(2), robot.drive_timeout(2), since=mark)

    update = build_order(oid, 1, [route.nodes[2]] + right, released=3, map_id=route.map_id,
                         first_seq=4)  # поехали направо
    mark = robot.send(update)
    s = robot.wait_accepted(update, mark)
    assert [n["nodeId"] for n in s["nodeStates"]] == [n.node_id for n in right], \
        "old horizon must be replaced by the update"

    s = robot.wait_idle(robot.drive_timeout(3))
    assert s["lastNodeId"] == right[-1].node_id
    left_ids = {n.node_id for n in left}
    assert not any(x["lastNodeId"] in left_ids for x in fms.states(mark)), "robot drove into the old horizon"


def test_horizon_can_be_dropped_by_update(robot):
    """Update из одного stitching node без horizon'а завершает order на decision point."""
    route = Route(robot.pose(), count=3)
    oid = new_order_id()
    order = build_order(oid, 0, route.nodes, released=2, map_id=route.map_id)  # base n0..n1
    mark = robot.send(order)
    robot.wait_accepted(order, mark)
    robot.wait_last_node(route.node_id(1), robot.drive_timeout(1), since=mark)

    update = build_order(oid, 1, [route.nodes[1]], released=1, map_id=route.map_id, first_seq=2)
    mark = robot.send(update)
    robot.wait_accepted(update, mark)
    s = robot.wait_idle(robot.t(5.0))
    assert s["lastNodeId"] == route.node_id(1)
