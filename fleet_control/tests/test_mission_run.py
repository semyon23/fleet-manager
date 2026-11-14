"""Миссии едут (M-3c): назначение роботу, исполнение через dispatch, статусы, отмена — с двумя роботами симулятора.

Нужен симулятор с роботами ktc/amr1 и ktc/amr2 (оба на карте map_1). Без него тесты пропускаются.
Карта map_1 — своя, на время теста; если map_1 уже есть в fms, тесты пропускаются: чужую карту не меняем.

    a ──4 м── b ──4 м── c ──────8 м────── d        дороги в обе стороны, 1 м/с
    amr1                                    amr2   так роботы стоят в начале каждого теста
"""

from __future__ import annotations

import time

import pytest

from conftest import SIM_MANUFACTURER, SIM_MAP, Missions
from test_availability import wait_availability

RESOLUTION = 0.05
NODES = {"a": 1, "b": 5, "c": 9, "d": 17}   # x, м; y у всех 2,5 м


def action(seconds: float) -> dict:
    """Действие симулятора: стоит на месте seconds секунд."""
    return {"actionType": "simulatedAction", "actionParameters": [{"key": "duration", "value": seconds}]}


def wait_mission(missions, mission_id: str, states: set[str], timeout: float = 40) -> dict:
    """Ждать, пока миссия в базе (то, что видит UI) окажется в одном из states."""
    deadline = time.monotonic() + timeout
    mission = None
    while time.monotonic() < deadline:
        code, mission = missions.from_database(mission_id)
        if code == 200 and mission["state"] in states:
            return mission
        time.sleep(0.2)
    raise TimeoutError(f"mission {mission_id} is not {states} after {timeout:.0f} s: {mission}")


def wait_robot(robots, robot_id: str, check, what: str, timeout: float = 10) -> dict:
    deadline = time.monotonic() + timeout
    robot = None
    while time.monotonic() < deadline:
        _, robot = robots.from_database(robot_id)
        if check(robot):
            return robot
        time.sleep(0.2)
    raise TimeoutError(f"robot {robot_id}: no {what} after {timeout:.0f} s: {robot}")


@pytest.fixture
def run_map(maps) -> dict:
    if maps.from_database(SIM_MAP)[0] == 200:
        pytest.skip(f"map {SIM_MAP} already exists in fms — not changing someone else's map")
    waypoints = [{"id": n, "u": round(x / RESOLUTION), "v": 50} for n, x in NODES.items()]
    pairs = [("a", "b"), ("b", "c"), ("c", "d")]
    edges = [{"id": f"{p}{q}", "from": p, "to": q} for p, q in pairs] + \
            [{"id": f"{q}{p}", "from": q, "to": p} for p, q in pairs]
    code, created = maps.create(id=SIM_MAP, width=400, height=100,
                                meta={"resolution": RESOLUTION, "origin": [0, 0, 0]},
                                waypoints=waypoints, edges=edges, stations=[])
    assert code == 201, created
    return created


@pytest.fixture
def two_robots(robots, run_map) -> tuple[str, str]:
    """amr1 на a, amr2 на d, оба IDLE."""
    ids = []
    for serial, node in (("amr1", "a"), ("amr2", "d")):
        code, robot = robots.register(manufacturer=SIM_MANUFACTURER, serialNumber=serial)
        if code == 409:
            pytest.fail(f"{SIM_MANUFACTURER}/{serial} is already registered under another id — delete it first")
        assert code == 201, robot
        try:
            robots.wait_status(robot["id"], lambda s: s["connection"] == "ONLINE", 10, "ONLINE")
        except TimeoutError:
            pytest.skip(f"simulator robot {SIM_MANUFACTURER}/{serial} is not ONLINE — is the simulator running?")
        robots.place(robot["id"], {"mapId": SIM_MAP, "nodeId": node}, lambda s, n=node: s["lastNodeId"] == n)
        ids.append(robot["id"])
    for robot_id in ids:
        wait_availability(robots, robot_id, "IDLE", timeout=10)
    return ids[0], ids[1]


# ------------------------------------------------------------------ миссия доезжает

def test_mission_completes(missions, robots, two_robots):
    """Доехать до b и постоять там 1 с: назначена, едет, выполнена; робот снова свободен на b."""
    amr1, _ = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b", "actions": [action(1)]}]))

    mission = wait_mission(missions, created["id"], {"COMPLETED", "FAILED", "CANCELED"})

    assert mission["state"] == "COMPLETED", mission
    assert mission["robotId"] == amr1                     # ближе к b
    assert mission["error"] is None
    assert mission["assignedAt"] and mission["startedAt"] and mission["finishedAt"]
    step = mission["stepStates"][0]
    assert step["state"] == "COMPLETED"
    assert step["startedAt"] and step["finishedAt"]
    assert [(a["actionType"], a["status"]) for a in step["actions"]] == [("simulatedAction", "FINISHED")]
    assert step["actions"][0]["actionId"]                 # его выдал dispatch

    robot = wait_robot(robots, amr1, lambda r: r["availability"]["state"] == "IDLE", "IDLE")
    assert robot["status"]["lastNodeId"] == "b"
    assert not robot["status"]["missionId"]


def test_nearest_robot_gets_the_mission(missions, two_robots):
    """К d ближе amr2 (он там стоит), к a — amr1."""
    amr1, amr2 = two_robots
    _, to_d = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "d"}]))
    assert wait_mission(missions, to_d["id"], {"COMPLETED"})["robotId"] == amr2

    _, to_a = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "a"}]))
    assert wait_mission(missions, to_a["id"], {"COMPLETED"})["robotId"] == amr1


def test_two_steps(missions, two_robots):
    """b с действием, потом обратно на a с действием — шаги выполняются по порядку."""
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b", "actions": [action(1)]},
                                                                {"nodeId": "a", "actions": [action(1)]}]))
    mission = wait_mission(missions, created["id"], {"COMPLETED", "FAILED", "CANCELED"})

    assert mission["state"] == "COMPLETED", mission
    first, second = mission["stepStates"]
    assert first["state"] == second["state"] == "COMPLETED"
    assert first["finishedAt"] <= second["startedAt"]


def test_requested_robot(missions, two_robots):
    """К b ближе amr1 (4 м против 12), но попросили amr2 — едет amr2."""
    _, amr2 = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr2, steps=[{"nodeId": "b"}]))
    mission = wait_mission(missions, created["id"], {"COMPLETED", "FAILED"})
    assert mission["robotId"] == amr2


# ------------------------------------------------------------------ очередь

def test_mission_waits_for_busy_robot(missions, two_robots):
    """Обе миссии — для amr1. Вторая ждёт в очереди и уходит ему, когда он закончит первую."""
    amr1, _ = two_robots
    _, first = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1,
                                             steps=[{"nodeId": "a", "actions": [action(3)]}]))
    wait_mission(missions, first["id"], {"RUNNING"})
    _, second = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, steps=[{"nodeId": "b"}]))

    assert wait_mission(missions, second["id"], {"QUEUED"})["robotId"] is None   # пока amr1 занят
    done = wait_mission(missions, first["id"], {"COMPLETED"})
    second = wait_mission(missions, second["id"], {"COMPLETED"})
    assert second["robotId"] == amr1
    assert second["assignedAt"] >= done["finishedAt"]


def test_higher_priority_goes_first(missions, two_robots):
    """amr1 занят; в очередь к нему встают LOW, а за ней HIGH. Освободившись, он берёт сначала HIGH."""
    amr1, _ = two_robots
    _, busy = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1,
                                            steps=[{"nodeId": "a", "actions": [action(3)]}]))
    wait_mission(missions, busy["id"], {"RUNNING"})
    _, low = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, priority="LOW", steps=[{"nodeId": "b"}]))
    _, high = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, priority="HIGH", steps=[{"nodeId": "a"}]))

    high = wait_mission(missions, high["id"], {"COMPLETED"}, timeout=60)
    low = wait_mission(missions, low["id"], {"COMPLETED"}, timeout=60)
    assert high["assignedAt"] < low["assignedAt"]


# ------------------------------------------------------------------ отмена

def test_cancel_running_mission(missions, robots, two_robots):
    """Действие на 30 с; отмена — сразу CANCELING, когда робот подтвердит — CANCELED; робот свободен."""
    amr1, _ = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1,
                                               steps=[{"nodeId": "a", "actions": [action(30)]}]))
    wait_mission(missions, created["id"], {"RUNNING"})

    code, cancelling = missions.cancel(created["id"])
    assert code == 202, cancelling
    assert cancelling["state"] in ("CANCELING", "CANCELED")

    mission = wait_mission(missions, created["id"], {"CANCELED", "FAILED", "COMPLETED"}, timeout=15)
    assert mission["state"] == "CANCELED", mission
    wait_robot(robots, amr1, lambda r: r["availability"]["state"] == "IDLE", "IDLE")

    code, error = missions.cancel(created["id"])
    assert code == 409


def test_cancelled_between_nodes_is_not_on_graph(missions, robots, two_robots):
    """Отменили на полпути a → d: симулятор встаёт сразу, посреди ребра. fms не считает его стоящим на узле
    и не даёт ему миссий, пока его не поставят на узел."""
    amr1, _ = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, steps=[{"nodeId": "d"}]))
    wait_mission(missions, created["id"], {"RUNNING"})
    wait_robot(robots, amr1, lambda r: r["status"]["driving"], "driving")
    time.sleep(2.5)                                      # отъехал от a, до b ещё не доехал

    missions.cancel(created["id"])
    wait_mission(missions, created["id"], {"CANCELED"}, timeout=15)

    robot = wait_robot(robots, amr1, lambda r: r["availability"]["state"] == "NOT_READY", "NOT_READY")
    messages = " ".join(reason["message"] for reason in robot["availability"]["reasons"])
    assert "between nodes" in messages, robot["availability"]

    # Поставили на узел — снова свободен
    robots.place(amr1, {"mapId": SIM_MAP, "nodeId": "b"}, lambda s: s["lastNodeId"] == "b")
    wait_robot(robots, amr1, lambda r: r["availability"]["state"] == "IDLE", "IDLE")


# ------------------------------------------------------------------ c1b: скоро свободный робот
#
# Робот, которому до конца миссии осталось только ехать, — кандидат наравне со свободными:
# его время = остаток миссии + путь от её конца. Новая миссия становится у него следующей (ASSIGNED)
# и уходит в dispatch, когда текущая COMPLETED.

def wait_next(missions, mission_id: str, robot_id: str, timeout: float = 10) -> dict:
    """Миссия назначена роботу, но ещё ждёт его (ASSIGNED, в dispatch не ушла)."""
    deadline = time.monotonic() + timeout
    mission = None
    while time.monotonic() < deadline:
        _, mission = missions.from_database(mission_id)
        if mission["state"] == "ASSIGNED" and mission["robotId"] == robot_id:
            return mission
        time.sleep(0.1)
    raise TimeoutError(f"mission {mission_id} is not next for {robot_id}: {mission}")


def drive_amr1_to_b(missions, robots, amr1: str) -> dict:
    """amr1 едет a → b без действий (4 м, ~5 с). Возвращает, когда он уже в пути."""
    _, current = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, steps=[{"nodeId": "b"}]))
    wait_mission(missions, current["id"], {"RUNNING"})
    wait_robot(robots, amr1, lambda r: r["status"]["driving"], "driving")
    return current


def test_soon_free_robot_beats_far_idle_robot(missions, robots, two_robots):
    """amr1 едет a → b, ему осталось ~5 с. amr2 свободен, но на d: до b 12 м. Миссия на b — следующая у amr1."""
    amr1, _ = two_robots
    current = drive_amr1_to_b(missions, robots, amr1)
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b", "actions": [action(1)]}]))

    wait_next(missions, created["id"], amr1)
    assert missions.from_database(current["id"])[1]["state"] == "RUNNING"   # назначена, пока он ещё едет

    done = wait_mission(missions, current["id"], {"COMPLETED"})
    mission = wait_mission(missions, created["id"], {"COMPLETED", "FAILED", "CANCELED"})
    assert mission["state"] == "COMPLETED", mission
    assert mission["robotId"] == amr1
    assert mission["assignedAt"] < done["finishedAt"]    # назначили до конца текущей
    assert mission["startedAt"] >= done["finishedAt"]    # а начали — после


def test_robot_with_actions_ahead_is_not_soon_free(missions, robots, two_robots):
    """amr1 едет на b, и там у него действие на 5 с — когда он освободится, неизвестно точно.
    Миссия на b уходит свободному amr2, хотя тот дальше."""
    amr1, amr2 = two_robots
    _, current = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1,
                                               steps=[{"nodeId": "b", "actions": [action(5)]}]))
    wait_mission(missions, current["id"], {"RUNNING"})
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b"}]))

    mission = wait_mission(missions, created["id"], {"COMPLETED", "FAILED", "CANCELED"})
    assert mission["robotId"] == amr2


def test_next_returns_to_queue_when_current_is_cancelled(missions, robots, two_robots):
    """Текущую миссию amr1 отменили в пути — он встал между узлами. Его следующая возвращается в очередь
    и достаётся amr2."""
    amr1, amr2 = two_robots
    current = drive_amr1_to_b(missions, robots, amr1)
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b"}]))
    wait_next(missions, created["id"], amr1)
    time.sleep(2)   # отъехал от a дальше 0,5 м: после отмены он не «на a», а между узлами

    missions.cancel(current["id"])
    wait_mission(missions, current["id"], {"CANCELED"}, timeout=15)

    mission = wait_mission(missions, created["id"], {"COMPLETED", "FAILED", "CANCELED"})
    assert mission["state"] == "COMPLETED", mission
    assert mission["robotId"] == amr2


def test_cancel_next_mission(missions, robots, two_robots):
    """Следующую миссию отменили, пока amr1 заканчивал текущую: сразу CANCELED, в dispatch она не уходит;
    текущая доезжает как ни в чём не бывало."""
    amr1, _ = two_robots
    current = drive_amr1_to_b(missions, robots, amr1)
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b"}]))
    wait_next(missions, created["id"], amr1)

    code, cancelled = missions.cancel(created["id"])
    assert code == 202, cancelled
    assert cancelled["state"] == "CANCELED"
    assert cancelled["startedAt"] is None

    assert wait_mission(missions, current["id"], {"COMPLETED", "FAILED", "CANCELED"})["state"] == "COMPLETED"
    robot = wait_robot(robots, amr1, lambda r: r["availability"]["state"] == "IDLE", "IDLE")
    assert robot["status"]["lastNodeId"] == "b"
    assert missions.from_database(created["id"])[1]["startedAt"] is None   # так и не начиналась


# ------------------------------------------------------------------ c2: маршруты для Live Map
#
# GET /routes control: у каждого робота с миссией — что ему осталось проехать по текущему шагу (released),
# цели шагов (goals) и следующая миссия (c1b). Пройденные узлы отрезаются.

A_TO_D = ["a", "b", "c", "d"]


def robot_route(missions, robot_id: str) -> dict | None:
    code, body = missions.routes(SIM_MAP)
    assert code == 200, body
    return next((route for route in body["items"] if route["robotId"] == robot_id), None)


def wait_route(missions, robot_id: str, check, what: str, timeout: float = 20) -> dict | None:
    deadline = time.monotonic() + timeout
    route = None
    while time.monotonic() < deadline:
        route = robot_route(missions, robot_id)
        if check(route):
            return route
        time.sleep(0.1)
    raise TimeoutError(f"robot {robot_id}: no {what} in /routes after {timeout:.0f} s: {route}")


def assert_path(released: dict, path: list[str]) -> None:
    """released — конец пути path (начало уже проехали), рёбра — между соседними узлами (id "ab", "bc"...)."""
    nodes = released["nodes"]
    assert nodes and nodes == path[-len(nodes):], released
    assert released["edges"] == [p + q for p, q in zip(nodes, nodes[1:])], released


def test_route_while_driving(missions, two_robots):
    """amr1 едет a → d (16 м): в /routes — его путь; проехал b — путь начинается с c; доехал — маршрута нет.
    Фильтр по карте: чужая карта — пусто, без mapId — все."""
    amr1, amr2 = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1, steps=[{"nodeId": "d"}]))

    route = wait_route(missions, amr1, lambda r: r is not None, "route")
    assert route["mapId"] == SIM_MAP
    assert route["missionId"] == created["id"]
    assert route["stepIndex"] == 0
    assert route["goals"] == ["d"]
    assert route["planned"] == {"nodes": [], "edges": []}   # трафика ещё нет — разрешено всё сразу
    assert route["nextMissionId"] is None
    assert_path(route["released"], A_TO_D)
    assert robot_route(missions, amr2) is None               # у amr2 миссии нет

    assert missions.routes("test-ctl-no-such-map") == (200, {"items": []})
    code, everything = missions.routes()
    assert code == 200
    assert any(r["robotId"] == amr1 for r in everything["items"])

    passed_b = wait_route(missions, amr1, lambda r: r is not None and r["released"]["nodes"][0] == "c", "c first")
    assert passed_b["released"] == {"nodes": ["c", "d"], "edges": ["cd"]}

    wait_mission(missions, created["id"], {"COMPLETED"})
    assert robot_route(missions, amr1) is None


def test_route_of_second_step(missions, two_robots):
    """Шаги: b с действием 2 с, потом a. Пока amr1 стоит на b — линии нет, цели b и a;
    потом — шаг 1, путь b → a и одна цель a."""
    amr1, _ = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1,
                                               steps=[{"nodeId": "b", "actions": [action(2)]}, {"nodeId": "a"}]))

    first = wait_route(missions, amr1, lambda r: r is not None, "route")
    assert first["stepIndex"] == 0
    assert first["goals"] == ["b", "a"]
    assert_path(first["released"], ["a", "b"])

    on_b = wait_route(missions, amr1, lambda r: r is not None and r["released"]["nodes"] == ["b"], "standing on b")
    assert on_b["stepIndex"] == 0
    assert on_b["released"]["edges"] == []

    back = wait_route(missions, amr1, lambda r: r is not None and r["stepIndex"] == 1, "step 1")
    assert back["goals"] == ["a"]
    assert_path(back["released"], ["b", "a"])

    wait_mission(missions, created["id"], {"COMPLETED"})
    assert robot_route(missions, amr1) is None


def test_route_shows_next_mission(missions, robots, two_robots):
    """amr1 заканчивает поездку a → b и уже получил следующую миссию: в его маршруте — nextMissionId,
    своего маршрута у следующей нет. Текущая закончилась — маршрут amr1 уже по следующей."""
    amr1, _ = two_robots
    current = drive_amr1_to_b(missions, robots, amr1)
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b", "actions": [action(2)]}]))
    wait_next(missions, created["id"], amr1)

    route = robot_route(missions, amr1)
    assert route["missionId"] == current["id"]
    assert route["nextMissionId"] == created["id"]
    _, body = missions.routes(SIM_MAP)
    assert all(r["missionId"] != created["id"] for r in body["items"])

    following = wait_route(missions, amr1, lambda r: r is not None and r["missionId"] == created["id"], "next mission")
    assert following["nextMissionId"] is None
    assert following["goals"] == ["b"]

    wait_mission(missions, created["id"], {"COMPLETED"})
    assert robot_route(missions, amr1) is None



# ------------------------------------------------------------------ c3: история миссии
#
# События пишутся вместе с документом миссии, одной транзакцией: когда в базе миссия COMPLETED,
# все её события до COMPLETED уже там. Сколько промежуточных статусов действия увидит dispatch, зависит
# от того, как часто робот шлёт state, — поэтому статусы действий проверяем по последнему, а не все.

def test_events_of_completed_mission(missions, two_robots):
    """Миссия на b с действием: QUEUED → ASSIGNED (робот и почему он) → RUNNING → шаг → действие → шаг → COMPLETED."""
    amr1, _ = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b", "actions": [action(1)]}]))
    wait_mission(missions, created["id"], {"COMPLETED"})

    events = Missions.events(created["id"])
    lines = Missions.brief(events)
    assert [line for line in lines if line.startswith("STATE")] == \
        ["STATE QUEUED", "STATE ASSIGNED", "STATE RUNNING", "STATE COMPLETED"]
    assert [line for line in lines if line.startswith("STEP")] == ["STEP 0 RUNNING", "STEP 0 COMPLETED"]
    actions = [line for line in lines if line.startswith("ACTION")]
    assert actions and actions[-1] == "ACTION FINISHED", lines

    # Порядок — как было: шаг начался после миссии, действие закончилось до шага, шаг — до миссии
    assert lines.index("STATE RUNNING") < lines.index("STEP 0 RUNNING") < lines.index("ACTION FINISHED") \
        < lines.index("STEP 0 COMPLETED") < lines.index("STATE COMPLETED") == len(lines) - 1

    assigned = events[lines.index("STATE ASSIGNED")]
    assert assigned["robotId"] == amr1
    assert assigned["reason"].endswith("s to the first step"), assigned
    assert events[0]["robotId"] is None                                  # QUEUED — ещё ничей
    assert all(event["robotId"] == amr1 for event in events[1:])
    assert [event["id"] for event in events] == sorted(event["id"] for event in events)
    assert [event["at"] for event in events] == sorted(event["at"] for event in events)
    action_event = next(event for event in events if event["type"] == "ACTION")
    assert (action_event["stepIndex"], action_event["actionType"]) == (0, "simulatedAction")
    assert action_event["actionId"]


def test_events_of_cancelled_running_mission(missions, two_robots):
    """Отмена во время действия: CANCELING с причиной, потом CANCELED — когда робот подтвердил."""
    amr1, _ = two_robots
    _, created = missions.create(Missions.body(SIM_MAP, requestedRobotId=amr1,
                                               steps=[{"nodeId": "a", "actions": [action(30)]}]))
    wait_mission(missions, created["id"], {"RUNNING"})
    missions.cancel(created["id"])
    wait_mission(missions, created["id"], {"CANCELED"}, timeout=15)

    events = Missions.events(created["id"])
    states = [event for event in events if event["type"] == "STATE"]
    assert [event["state"] for event in states][-2:] == ["CANCELING", "CANCELED"]
    assert states[-2]["reason"] == "canceled by operator"
    assert Missions.brief(events)[-1] == "STATE CANCELED"


def test_events_of_next_mission_returned_to_queue(missions, robots, two_robots):
    """c1b: следующая у amr1 — ASSIGNED «after its current mission»; его текущую отменили — QUEUED с причиной
    (робот события — amr1, от которого она ушла); потом её берёт amr2 и выполняет."""
    amr1, amr2 = two_robots
    current = drive_amr1_to_b(missions, robots, amr1)
    _, created = missions.create(Missions.body(SIM_MAP, steps=[{"nodeId": "b"}]))
    wait_next(missions, created["id"], amr1)
    time.sleep(2)   # отъехал от a дальше 0,5 м: после отмены он не «на a», а между узлами

    missions.cancel(current["id"])
    wait_mission(missions, created["id"], {"COMPLETED"})

    states = [event for event in Missions.events(created["id"]) if event["type"] == "STATE"]
    assert [event["state"] for event in states] == \
        ["QUEUED", "ASSIGNED", "QUEUED", "ASSIGNED", "RUNNING", "COMPLETED"]
    assert (states[1]["robotId"], states[1]["reason"].startswith("after its current mission")) == (amr1, True)
    assert (states[2]["robotId"], states[2]["reason"]) == (amr1, "the current mission of the robot did not complete")
    assert states[3]["robotId"] == amr2
