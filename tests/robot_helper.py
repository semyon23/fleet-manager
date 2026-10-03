"""Типичные шаги тестов поверх FmsClient: дождаться idle, отправить order и
дождаться его принятия или отказа, вернуть робота в исходное состояние."""

from __future__ import annotations

import time

from fms_client import FmsClient, has_error, is_idle, rejection_errors
from order_builder import Pose, RouteNode, build_order, new_order_id


class RobotHelper:
    def __init__(self, fms: FmsClient, timeout_scale: float, seconds_per_meter: float):
        self.fms = fms
        self.scale = timeout_scale
        self.seconds_per_meter = seconds_per_meter

    # ------------------------------------------------------------- время

    def t(self, seconds: float) -> float:
        return seconds * self.scale

    def drive_timeout(self, meters: float) -> float:
        return self.t(5.0 + meters * self.seconds_per_meter)

    # ------------------------------------------------------------ состояние

    def state(self) -> dict:
        s = self.fms.latest("state")
        assert s is not None, "robot has not published any state"
        return s

    def pose(self) -> Pose:
        return Pose.from_state(self.state())

    def since_latest(self) -> int:
        """Индекс последнего state: условие проверится и на нём, и на новых."""
        return max(0, self.fms.mark() - 1)

    def wait_idle(self, timeout: float) -> dict:
        return self.fms.wait_state(lambda s: is_idle(s) and not s["driving"], timeout,
                                   since=self.since_latest(), what="robot idle and standing")

    # --------------------------------------------------------------- order'а

    def send(self, order: dict) -> int:
        """Отправить order; возвращает отметку журнала state до отправки."""
        mark = self.fms.mark()
        self.fms.send_order(order)
        return mark

    def wait_accepted(self, order: dict, since: int, timeout: float | None = None) -> dict:
        oid, uid = order["orderId"], order["orderUpdateId"]
        return self.fms.wait_state(
            lambda s: s["orderId"] == oid and s["orderUpdateId"] == uid and not rejection_errors(s),
            timeout or self.t(3.0), since=since,
            what=f"order {oid}/{uid} accepted (orderId, orderUpdateId in state, no rejection errors)")

    def wait_error(self, error_type: str, since: int, timeout: float | None = None) -> dict:
        return self.fms.wait_state(lambda s: has_error(s, error_type), timeout or self.t(3.0),
                                   since=since, what=f"error {error_type} in state")

    def wait_last_node(self, node_id: str, timeout: float, since: int = 0) -> dict:
        return self.fms.wait_state(lambda s: s["lastNodeId"] == node_id, timeout, since=since,
                                   what=f"lastNodeId == {node_id}")

    def assert_stays(self, predicate, seconds: float, what: str) -> None:
        """Условие держится всё время: проверяем каждый state за seconds секунд."""
        since = self.since_latest()
        time.sleep(self.t(seconds))
        for s in self.fms.states(since):
            assert predicate(s), f"{what} — violated in state headerId={s.get('headerId')}"

    # ------------------------------------------------------- приведение в порядок

    def clear_order_errors(self) -> None:
        """Ошибки отказа висят до следующего принятого order'а (6.1.4).
        Order из одного узла там, где робот стоит, принимается и сразу завершается."""
        pose = self.pose()
        order = build_order(new_order_id("reset"), 0,
                            [RouteNode(f"reset_{int(time.time() * 1000)}", pose.x, pose.y)],
                            released=1, map_id=pose.map_id)
        mark = self.send(order)
        self.wait_accepted(order, mark)
        self.wait_idle(self.t(3.0))

    def finish_waiting_order(self) -> None:
        """Робот стоит на decision point с horizon'ом (прошлый тест упал на середине).
        Update из одного stitching node без horizon'а завершает order."""
        s = self.state()
        released_ahead = any(n.get("released") for n in s["nodeStates"])
        if is_idle(s) or released_ahead or not s["lastNodeId"]:
            return
        pose = self.pose()
        order = build_order(s["orderId"], s["orderUpdateId"] + 1,
                            [RouteNode(s["lastNodeId"], pose.x, pose.y)], released=1,
                            map_id=pose.map_id, first_seq=s["lastNodeSequenceId"])
        self.fms.send_order(order)
