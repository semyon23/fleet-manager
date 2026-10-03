"""Отказ от order'ов (6.1.4, рисунок 8).

Общее для всех отказов: order не берётся, текущий order (если есть) продолжается,
в state.errors появляется ошибка нужного типа и висит до следующего принятого order'а.
"""

import json

import pytest

from fms_client import rejection_errors
from order_builder import Pose, Route, build_order, new_order_id


def assert_order_unchanged(s, order_id, update_id):
    assert (s["orderId"], s["orderUpdateId"]) == (order_id, update_id), \
        f"rejected order must not be taken over: state has {s['orderId']}/{s['orderUpdateId']}"


# ------------------------------------------------------------------ неверный формат

def test_malformed_json(robot):
    before = robot.state()
    mark = robot.fms.mark()
    robot.fms.send_raw("order", "{this is not json")
    s = robot.wait_error("VALIDATION_FAILURE", mark)
    assert_order_unchanged(s, before["orderId"], before["orderUpdateId"])


def test_missing_mandatory_field(robot):
    before = robot.state()
    route = Route(robot.pose(), count=1)
    order = build_order(new_order_id(), 0, route.nodes, released=2, map_id=route.map_id)
    del order["nodes"]
    s = robot.wait_error("VALIDATION_FAILURE", robot.send(order))
    assert_order_unchanged(s, before["orderId"], before["orderUpdateId"])


def test_wrong_number_of_edges(robot):
    route = Route(robot.pose(), count=2)
    order = build_order(new_order_id(), 0, route.nodes, released=3, map_id=route.map_id)
    order["edges"].pop()
    robot.wait_error("VALIDATION_FAILURE", robot.send(order))


def test_released_node_after_horizon(robot):
    route = Route(robot.pose(), count=2)
    order = build_order(new_order_id(), 0, route.nodes, released=3, map_id=route.map_id)
    order["nodes"][1]["released"] = False
    order["edges"][0]["released"] = False  # n1 и ребро к нему — horizon, а n2 released
    robot.wait_error("VALIDATION_FAILURE", robot.send(order))


def test_unknown_enum_value(robot):
    route = Route(robot.pose(), count=1)
    order = build_order(new_order_id(), 0, route.nodes, released=2, map_id=route.map_id)
    order["edges"][0]["orientationType"] = "SIDEWAYS"  # допустимо только GLOBAL / TANGENTIAL
    s = robot.wait_error("VALIDATION_FAILURE", robot.send(order))
    assert s["orderId"] != order["orderId"]


# ------------------------------------------------------------ неверное содержимое

def test_start_node_out_of_range(robot):
    pose = robot.pose()
    far = Pose(pose.x + 5.0, pose.y + 5.0, pose.theta, pose.map_id)  # ~7 м от робота
    route = Route(far, count=1)
    order = build_order(new_order_id(), 0, route.nodes, released=2, map_id=route.map_id)
    s = robot.wait_error("START_NODE_OUT_OF_RANGE", robot.send(order))
    assert s["orderId"] != order["orderId"]


def test_unknown_map(robot):
    if "maps" not in robot.state():
        pytest.skip("robot does not report maps")
    route = Route(robot.pose(), count=1)
    order = build_order(new_order_id(), 0, route.nodes, released=2, map_id="no_such_map")
    robot.wait_error("UNKNOWN_MAP_ID", robot.send(order))


def test_new_order_must_start_with_update_id_zero(robot):
    """Рисунок 8, шаг 4. Тип ошибки спецификация не задаёт — проверяем только отказ."""
    route = Route(robot.pose(), count=1)
    order = build_order(new_order_id(), 1, route.nodes, released=2, map_id=route.map_id)
    mark = robot.send(order)
    s = robot.fms.wait_state(lambda x: bool(rejection_errors(x)), robot.t(3.0), since=mark,
                             what="order with orderUpdateId 1 rejected")
    assert s["orderId"] != order["orderId"]


# ------------------------------------------------------- конфликт с текущим order'ом

def test_other_order_active(robot):
    route = Route(robot.pose(), count=4)
    active = build_order(new_order_id("active"), 0, route.nodes, released=5, map_id=route.map_id)
    mark = robot.send(active)
    robot.wait_accepted(active, mark)

    pose = robot.pose()
    other = build_order(new_order_id("other"), 0, Route(pose, count=1).nodes, released=2,
                        map_id=pose.map_id)
    s = robot.wait_error("OTHER_ORDER_ACTIVE", robot.send(other))
    assert_order_unchanged(s, active["orderId"], 0)

    # Текущий order продолжается и выполняется до конца.
    s = robot.wait_idle(robot.drive_timeout(4))
    assert (s["orderId"], s["lastNodeId"]) == (active["orderId"], route.node_id(4))


@pytest.fixture
def waiting_order(robot):
    """Order с horizon'ом, робот стоит на decision point n2 (sequenceId 4)."""
    route = Route(robot.pose(), count=3)
    oid = new_order_id()
    order = build_order(oid, 0, route.nodes, released=3, map_id=route.map_id)
    mark = robot.send(order)
    robot.wait_accepted(order, mark)
    robot.wait_last_node(route.node_id(2), robot.drive_timeout(2), since=mark)
    return route, oid


def finish(robot, route, oid, update_id):
    update = build_order(oid, update_id, route.nodes[2:], released=2, map_id=route.map_id, first_seq=4)
    mark = robot.send(update)
    robot.wait_accepted(update, mark)
    s = robot.wait_idle(robot.drive_timeout(1))
    assert s["lastNodeId"] == route.node_id(3)


def test_outdated_order_update(robot, waiting_order):
    route, oid = waiting_order
    # update 1: base не продлеваем, horizon тот же — робот по-прежнему ждёт
    keep = build_order(oid, 1, route.nodes[2:], released=1, map_id=route.map_id, first_seq=4)
    mark = robot.send(keep)
    robot.wait_accepted(keep, mark)

    old = build_order(oid, 0, route.nodes, released=4, map_id=route.map_id)  # orderUpdateId 0 < 1
    s = robot.wait_error("OUTDATED_ORDER_UPDATE", robot.send(old))
    assert_order_unchanged(s, oid, 1)
    robot.assert_stays(lambda x: x["lastNodeId"] == route.node_id(2), 1.0,
                       "outdated update must not move the robot")
    finish(robot, route, oid, 2)


def test_same_update_id_with_different_content(robot, waiting_order):
    route, oid = waiting_order
    changed = build_order(oid, 0, route.nodes, released=4, map_id=route.map_id)  # тот же id, base длиннее
    s = robot.wait_error("SAME_ORDER_UPDATE_ID", robot.send(changed))
    assert_order_unchanged(s, oid, 0)
    robot.assert_stays(lambda x: x["lastNodeId"] == route.node_id(2), 1.0,
                       "rejected update must not move the robot")
    finish(robot, route, oid, 1)


def test_update_not_starting_at_decision_point(robot, waiting_order):
    """Рисунок 8, шаги 9/10: первый узел update'а — decision point. Тип ошибки не задан."""
    route, oid = waiting_order
    wrong = build_order(oid, 1, route.nodes[1:], released=3, map_id=route.map_id, first_seq=2)  # с n1
    mark = robot.send(wrong)
    s = robot.fms.wait_state(lambda x: bool(rejection_errors(x)), robot.t(3.0), since=mark,
                             what="update not starting at decision point rejected")
    assert_order_unchanged(s, oid, 0)
    finish(robot, route, oid, 1)


# -------------------------------------------------------------- жизнь ошибок

def test_rejection_error_cleared_by_next_accepted_order(robot):
    mark = robot.fms.mark()
    robot.fms.send_raw("order", json.dumps({"broken": True}))
    robot.wait_error("VALIDATION_FAILURE", mark)

    route = Route(robot.pose(), count=1)
    order = build_order(new_order_id(), 0, route.nodes, released=2, map_id=route.map_id)
    mark = robot.send(order)
    s = robot.wait_accepted(order, mark)  # wait_accepted требует отсутствия ошибок отказа
    assert not rejection_errors(s)
    robot.wait_idle(robot.drive_timeout(1))
