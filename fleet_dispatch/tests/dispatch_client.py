"""Помощники для тестов dispatch: стримы статусов в фоне и сборка миссий.

Стрим читается в отдельном потоке и складывает все сообщения в журнал. Тест ждёт нужное
сообщение через wait(): условие проверяется для сообщений начиная с отметки since,
поэтому старые сообщения не дают ложных срабатываний (как FmsClient в тестах симулятора).
"""

from __future__ import annotations

import threading
import time
import uuid
from typing import Callable, Optional

import grpc

import fms_types_pb2 as fms


class UpdateStream:
    def __init__(self, call):
        self._call = call
        self._items: list = []
        self._cond = threading.Condition()
        self.error: Optional[grpc.RpcError] = None
        self.closed = False
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        try:
            for item in self._call:
                with self._cond:
                    self._items.append(item)
                    self._cond.notify_all()
        except grpc.RpcError as e:
            with self._cond:
                self.error = e
        finally:
            with self._cond:
                self.closed = True
                self._cond.notify_all()

    def mark(self) -> int:
        with self._cond:
            return len(self._items)

    def items(self, since: int = 0) -> list:
        with self._cond:
            return list(self._items[since:])

    def wait(self, predicate: Callable, timeout: float, since: int = 0, what: str = ""):
        deadline = time.monotonic() + timeout
        index = since
        with self._cond:
            while True:
                while index < len(self._items):
                    item = self._items[index]
                    index += 1
                    if predicate(item):
                        return item
                if self.closed:
                    raise AssertionError(f"stream closed while waiting for {what}: {self.error}")
                left = deadline - time.monotonic()
                if left <= 0:
                    last = self._items[-1] if self._items else None
                    raise TimeoutError(f"timeout {timeout:.1f}s waiting for {what}\nlast message: {last}")
                self._cond.wait(left)

    def wait_closed(self, timeout: float) -> Optional[grpc.RpcError]:
        """Ждать, пока dispatch закроет стрим. Возвращает ошибку, с которой он закрылся."""
        with self._cond:
            if not self._cond.wait_for(lambda: self.closed, timeout):
                raise TimeoutError(f"stream was not closed in {timeout:.1f}s")
            return self.error

    def close(self) -> None:
        self._call.cancel()


# ------------------------------------------------------------------ условия для wait()

def robot_status(robot_id: str, condition: Callable = lambda s: True) -> Callable:
    """Сообщение стрима роботов — статус этого робота, для которого выполняется condition."""
    return lambda u: u.HasField("status") and u.status.robot_id == robot_id and condition(u.status)


def mission_status(mission_id: str, condition: Callable = lambda s: True) -> Callable:
    return lambda u: u.HasField("status") and u.status.mission_id == mission_id and condition(u.status)


FINAL_MISSION_STATES = (fms.MISSION_STATE_COMPLETED, fms.MISSION_STATE_FAILED, fms.MISSION_STATE_CANCELED)


def is_final(status) -> bool:
    return status.state in FINAL_MISSION_STATES


# ------------------------------------------------------------------------ миссии

def new_id(prefix: str) -> str:
    """orderId строится из mission_id, а выполненный orderId робот второй раз не примет —
    поэтому у каждой миссии теста свой id."""
    return f"{prefix}-{uuid.uuid4().hex[:8]}"


def sim_action(seconds: float, blocking: int = fms.BLOCKING_TYPE_HARD) -> fms.Action:
    """simulatedAction симулятора: длится seconds секунд."""
    action = fms.Action(action_type="simulatedAction", blocking_type=blocking)
    parameter = action.parameters.add(key="duration")
    parameter.value.number_value = seconds
    return action


def node(node_id: str, x: float, y: float = 0.0, map_id: str = "map_1", actions=()) -> fms.RouteNode:
    route_node = fms.RouteNode(node_id=node_id, x=x, y=y, map_id=map_id)
    route_node.actions.extend(actions)
    return route_node


def move_mission(mission_id: str, robot_id: str, nodes: list, released_nodes: Optional[int] = None) -> fms.Mission:
    """Миссия из одного шага s1: проехать по nodes. Рёбра e0, e1, ... между соседними узлами."""
    step = fms.MoveStep()
    step.nodes.extend(nodes)
    step.nodes[0].allowed_deviation_xy = 0.5
    for i in range(len(nodes) - 1):
        step.edges.add(edge_id=f"e{i}")
    if released_nodes is not None:
        step.released_nodes = released_nodes
    mission = fms.Mission(mission_id=mission_id, robot_id=robot_id)
    mission.nodes.add(id="s1", name="move", move=step)
    return mission


def action_mission(mission_id: str, robot_id: str, actions: list) -> fms.Mission:
    """Миссия из одного шага s1: действия на месте."""
    step = fms.ActionStep()
    step.actions.extend(actions)
    mission = fms.Mission(mission_id=mission_id, robot_id=robot_id)
    mission.nodes.add(id="s1", name="action", action=step)
    return mission


# ------------------------------------------------------------- многошаговые миссии

def _step(step_id: str, on_failure: Optional[int] = None, max_retries: int = 0,
          timeout: Optional[float] = None, **kind) -> fms.MissionNode:
    """Шаг миссии. kind — один из move= / action= / wait=; timeout — в секундах."""
    step = fms.MissionNode(id=step_id, name=step_id, **kind)
    if on_failure is not None:
        step.on_failure = on_failure
    step.max_retries = max_retries
    if timeout is not None:
        step.timeout.FromMilliseconds(int(timeout * 1000))
    return step


def step_move(step_id: str, nodes: list, **options) -> fms.MissionNode:
    move = fms.MoveStep()
    move.nodes.extend(nodes)
    move.nodes[0].allowed_deviation_xy = 0.5
    for i in range(len(nodes) - 1):
        move.edges.add(edge_id=f"{step_id}-e{i}")
    return _step(step_id, move=move, **options)


def step_action(step_id: str, actions: list, **options) -> fms.MissionNode:
    return _step(step_id, action=fms.ActionStep(actions=actions), **options)


def step_wait(step_id: str, seconds: float, **options) -> fms.MissionNode:
    wait = fms.WaitStep()
    wait.duration.FromMilliseconds(int(seconds * 1000))
    return _step(step_id, wait=wait, **options)


def mission_of(mission_id: str, robot_id: str, steps: list) -> fms.Mission:
    mission = fms.Mission(mission_id=mission_id, robot_id=robot_id)
    mission.nodes.extend(steps)
    return mission


def unknown_action() -> fms.Action:
    """Действие, которого нет в factsheet симулятора: робот отклонит order с ним."""
    return fms.Action(action_type="noSuchAction", blocking_type=fms.BLOCKING_TYPE_HARD)
