"""Построение order'ов для тестов.

Маршрут строится от текущей позиции робота (из state), прямой линией вдоль
его курса. Первый узел — ровно там, где робот стоит, поэтому он всегда
тривиально достижим (6.1.1), и тестам не нужна заранее известная карта.
"""

from __future__ import annotations

import math
import uuid
from dataclasses import dataclass


@dataclass
class Pose:
    x: float
    y: float
    theta: float
    map_id: str

    @staticmethod
    def from_state(state: dict) -> "Pose":
        p = state["mobileRobotPosition"]
        return Pose(p["x"], p["y"], p.get("theta", 0.0), p["mapId"])


@dataclass
class RouteNode:
    node_id: str
    x: float
    y: float


class Route:
    """Узлы n0..nK по прямой: n0 — текущая позиция, шаг step метров."""

    def __init__(self, start: Pose, count: int, step: float = 1.0, prefix: str | None = None,
                 heading: float | None = None):
        self.map_id = start.map_id
        self.prefix = prefix or f"t{uuid.uuid4().hex[:6]}"
        h = start.theta if heading is None else heading
        self.nodes = [
            RouteNode(f"{self.prefix}_n{i}", start.x + i * step * math.cos(h),
                      start.y + i * step * math.sin(h))
            for i in range(count + 1)
        ]

    def node_id(self, i: int) -> str:
        return self.nodes[i].node_id

    def branch(self, from_index: int, count: int, step: float, heading: float,
               tag: str) -> list[RouteNode]:
        """Ответвление от узла from_index (для теста замены horizon'а)."""
        base = self.nodes[from_index]
        return [RouteNode(f"{self.prefix}_{tag}{i}", base.x + i * step * math.cos(heading),
                          base.y + i * step * math.sin(heading))
                for i in range(1, count + 1)]


def node_json(node: RouteNode, seq: int, released: bool, map_id: str,
              deviation: float = 0.5) -> dict:
    return {
        "nodeId": node.node_id,
        "sequenceId": seq,
        "released": released,
        "nodePosition": {
            "x": node.x,
            "y": node.y,
            "mapId": map_id,
            "allowedDeviationXY": {"a": deviation, "b": deviation, "theta": 0.0},
            "allowedDeviationTheta": math.pi,
        },
        "actions": [],
    }


def edge_json(a: RouteNode, b: RouteNode, seq: int, released: bool) -> dict:
    return {
        "edgeId": f"{a.node_id}-{b.node_id}",
        "sequenceId": seq,
        "released": released,
        "actions": [],
    }


def build_order(order_id: str, update_id: int, nodes: list[RouteNode], released: int,
                map_id: str, first_seq: int = 0) -> dict:
    """Order из узлов nodes; первые `released` узлов (и рёбра между ними) — base, остальное — horizon.

    first_seq — sequenceId первого узла: 0 для нового order'а,
    sequenceId decision point'а для update'а.
    """
    assert 1 <= released <= len(nodes)
    node_list = [node_json(n, first_seq + 2 * i, i < released, map_id) for i, n in enumerate(nodes)]
    edge_list = [edge_json(nodes[i], nodes[i + 1], first_seq + 2 * i + 1, i + 1 < released)
                 for i in range(len(nodes) - 1)]
    return {"orderId": order_id, "orderUpdateId": update_id,
            "nodes": node_list, "edges": edge_list}


def new_order_id(prefix: str = "order") -> str:
    return f"{prefix}-{uuid.uuid4().hex[:8]}"
