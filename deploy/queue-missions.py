#!/usr/bin/env python3
"""Параллельная отправка миссий в COMPASS через HTTP API."""

import argparse
import json
import re
from concurrent.futures import ThreadPoolExecutor, as_completed
from http.client import HTTPException
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
from uuid import uuid4


def mission_argument(value):
    robot, separator, route = value.partition(":")
    nodes = [node.strip() for node in route.split(",")]
    if not separator or not robot.strip() or not all(nodes):
        raise argparse.ArgumentTypeError("ожидается РОБОТ:УЗЕЛ[,УЗЕЛ...] или -:УЗЕЛ")
    return robot.strip(), nodes


def queue_mission(endpoint, mission):
    request = Request(
        endpoint,
        data=json.dumps(mission, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
        method="POST",
    )
    try:
        with urlopen(request, timeout=10) as response:
            result = json.load(response)
            if not isinstance(result, dict) or result.get("id") != mission["id"]:
                return False, "сервер вернул неожиданный ответ, проверьте миссию по ID"
            return True, f"HTTP {response.status} {result.get('state', '')}".rstrip()
    except HTTPError as error:
        with error:
            try:
                details = json.load(error)
                message = details.get("message") if isinstance(details, dict) else None
            except (HTTPException, OSError, ValueError):
                message = None
        return False, f"HTTP {error.code} {message or error.reason}"
    except (HTTPException, OSError, ValueError) as error:
        return False, f"исход запроса не подтверждён — {error}"


def main():
    parser = argparse.ArgumentParser(
        description="Поставить несколько миссий COMPASS в очередь параллельными запросами.",
        epilog="Для автовыбора робота добавьте -- перед списком, например -- '-:n012'.",
    )
    parser.add_argument("--map", required=True, help="ID карты")
    parser.add_argument("--url", default="http://127.0.0.1:8080", help="адрес шлюза FMS")
    parser.add_argument("--batch-id", help="ID запуска для безопасного повтора тех же запросов")
    parser.add_argument("--parallel", type=int, default=8, help="число параллельных запросов")
    parser.add_argument("--priority", choices=("LOW", "NORMAL", "HIGH", "CRITICAL"), default="NORMAL")
    parser.add_argument("--dry-run", action="store_true", help="показать команды без отправки")
    parser.add_argument("missions", nargs="+", type=mission_argument, metavar="РОБОТ:УЗЕЛ[,УЗЕЛ...]")
    args = parser.parse_args()

    try:
        url = urlsplit(args.url)
    except ValueError as error:
        parser.error(f"некорректный --url — {error}")
    if (
        url.scheme not in ("http", "https")
        or not url.hostname
        or url.path not in ("", "/")
        or url.query
        or url.fragment
    ):
        parser.error("--url должен быть адресом шлюза вида http://127.0.0.1:8080")
    if args.parallel < 1:
        parser.error("--parallel должен быть положительным")

    batch_id = args.batch_id if args.batch_id is not None else f"batch-{uuid4().hex[:16]}"
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,50}", batch_id):
        parser.error("--batch-id должен содержать от 1 до 50 букв A–Z, цифр, дефисов или подчёркиваний")

    payloads = []
    for index, (robot, nodes) in enumerate(args.missions, 1):
        mission_id = f"{batch_id}-{index}"
        mission = {
            "id": mission_id,
            "name": mission_id,
            "mapId": args.map,
            "priority": args.priority,
            "steps": [{"nodeId": node} for node in nodes],
        }
        if robot != "-":
            mission["requestedRobotId"] = robot
        payloads.append(mission)

    print(f"ID запуска — {batch_id}", flush=True)
    if args.dry_run:
        print(json.dumps(payloads, ensure_ascii=False, indent=2))
        return 0

    endpoint = f"{args.url.rstrip('/')}/api/fms/missions"
    success = True
    with ThreadPoolExecutor(max_workers=min(args.parallel, len(payloads))) as pool:
        pending = {pool.submit(queue_mission, endpoint, mission): mission for mission in payloads}
        for future in as_completed(pending):
            mission = pending[future]
            ok, message = future.result()
            robot = mission.get("requestedRobotId", "автовыбор")
            route = " → ".join(step["nodeId"] for step in mission["steps"])
            print(f"{mission['id']}  {robot} → {route}  {message}", flush=True)
            success = success and ok
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
