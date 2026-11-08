"""Карты через control (M-1): POST / PATCH / DELETE — контракт UI, раздел 6.

Пишут тесты через REST control, проверяют то, что увидит UI, — REST базы.
Нужны control и база; dispatch и симулятор — только для удаления карты, на которой стоит робот.
"""

from __future__ import annotations

import re
import threading

import pytest

from conftest import CONTROL, SIM_MAP, http

IMAGE = "data:image/png;base64,iVBORw0KGgo="


def error_text(body: dict) -> str:
    return " | ".join(body["details"]["errors"])


# ------------------------------------------------------------------ POST /maps

def test_create_map(maps):
    code, created = maps.create(name="Склад «A»", pgmDataUrl=IMAGE)

    assert code == 201
    assert created["version"] == 1
    assert created["name"] == "Склад «A»"
    assert created["pgmDataUrl"] == IMAGE
    assert created["waypoints"] == maps.document()["waypoints"]

    code, stored = maps.from_database(created["id"])
    assert code == 200
    assert stored == created          # UI читает из базы ровно то, что вернул POST


def test_create_with_robot_map_id(maps):
    """id карты — это mapId, которым пользуются роботы: задаётся при создании."""
    code, created = maps.create(id="test-ctl-map_1")
    assert code == 201
    assert created["id"] == "test-ctl-map_1"


def test_create_generates_id(maps):
    code, created = maps.create(id=None)
    assert code == 201
    assert re.fullmatch(r"map-[a-z0-9]{6}", created["id"])


def test_create_taken_id(maps):
    _, created = maps.create()
    code, body = http("POST", f"{CONTROL}/api/fms/maps", maps.document(id=created["id"], name="other"))
    assert code == 409
    assert body["code"] == "ALREADY_EXISTS"
    assert maps.from_database(created["id"])[1]["name"] == "Test map"


@pytest.mark.parametrize("fields, field", [
    ({"name": None}, "name"),
    ({"name": 5}, "name"),
    ({"id": "bad id!"}, "id"),
    ({"pgmDataUrl": 5}, "pgmDataUrl"),
])
def test_create_invalid_fields(maps, fields, field):
    code, body = maps.create(**fields)
    assert code == 400
    assert body["code"] == "INVALID_INPUT"
    assert field in body["details"]


def test_create_not_json(maps):
    code, body = http("POST", f"{CONTROL}/api/fms/maps", "{not json")
    assert code == 400
    assert body["code"] == "INVALID_INPUT"


BAD_MAPS = [
    ({"width": None}, "width must be a positive number"),
    ({"meta": {"resolution": 0, "origin": [0, 0]}}, "meta.resolution"),
    ({"meta": {"resolution": 0.05, "origin": [0]}}, "meta.origin"),
    ({"waypoints": "n1"}, "waypoints must be an array"),
    ({"waypoints": [{"u": 1, "v": 2}], "edges": []}, "waypoint without a string id"),
    ({"waypoints": [{"id": "n1", "u": "1", "v": 2}], "edges": []}, "waypoint n1: u and v must be numbers"),
    ({"waypoints": [{"id": "st1", "u": 1, "v": 2}], "edges": []}, "duplicate node id: st1"),
    ({"edges": [{"id": "e1", "from": "n1", "to": "n2"}, {"id": "e1", "from": "n2", "to": "n1"}]},
     "duplicate edge id: e1"),
    ({"edges": [{"id": "e1", "from": "n1", "to": "nope"}]}, "edge e1: to-node not found"),
    ({"edges": [{"from": "n1", "to": "n2"}]}, "edge without a string id"),
    ({"edges": [{"id": "e1", "from": "n1", "to": "n2", "maxSpeed": -1}]}, "edge e1: maxSpeed"),
    ({"edges": [{"id": "e1", "from": "n1", "to": "n2", "maxSpeed": 0}]}, "edge e1: maxSpeed must be a number > 0"),
    ({"stations": [{"id": "st1", "u": 1, "v": 1, "interactionNodeIds": ["nope"]}]},
     "station st1: interaction node \"nope\" not found"),
]


@pytest.mark.parametrize("fields, error", BAD_MAPS, ids=[e for _, e in BAD_MAPS])
def test_create_invalid_map(maps, fields, error):
    code, body = maps.create(**fields)
    assert code == 400
    assert body["code"] == "INVALID_MAP"
    assert error in error_text(body)
    assert maps.from_database(maps.ids[-1])[0] == 404     # ничего не сохранено


def test_all_map_errors_at_once(maps):
    code, body = maps.create(width=0, edges=[{"id": "e1", "from": "a", "to": "b"}])
    assert code == 400
    assert len(body["details"]["errors"]) == 3            # width, from, to
    assert body["message"] == body["details"]["errors"][0]


def test_warnings_do_not_block(maps):
    """Изолированный узел, петля, cost < 0, станция без подъезда — как во фронте, только предупреждения,
    подтверждения не требуют. Дорога n1 ↔ n2 — в обе стороны: одностороннее ребро без обратного пути
    было бы ловушкой, а это уже 409 (test_trap_needs_confirmation)."""
    code, created = maps.create(
        waypoints=[{"id": "n1", "u": 10, "v": 20}, {"id": "n2", "u": 50, "v": 20}, {"id": "lonely", "u": 1, "v": 1}],
        edges=[{"id": "e1", "from": "n1", "to": "n2", "cost": -5}, {"id": "e2", "from": "n2", "to": "n1"},
               {"id": "loop", "from": "n1", "to": "n1"}],
        stations=[{"id": "st1", "u": 60, "v": 30}],
    )
    assert code == 201, created


def test_empty_map(maps):
    """Новая карта в редакторе — без узлов; списки по умолчанию — []."""
    code, created = maps.create(waypoints=None, edges=None, stations=None, zones=None, assignedRobots=None)
    assert code == 201, created
    for key in ("waypoints", "edges", "stations", "zones", "assignedRobots"):
        assert created[key] == []


def test_unknown_fields_are_kept(maps):
    """Документ хранится как есть: поля, о которых сервер не знает, не теряются."""
    waypoints = [{"id": "n1", "u": 10, "v": 20, "name": "Dock", "stopAllowed": False},
                 {"id": "n2", "u": 50, "v": 20}]
    _, created = maps.create(waypoints=waypoints, description="first floor")
    _, stored = maps.from_database(created["id"])
    assert stored["description"] == "first floor"
    assert stored["waypoints"] == waypoints


def test_record_fields_are_set_by_server(maps):
    _, created = maps.create(version=99, createdAt="2000-01-01T00:00:00.000Z", zoneSet={"zoneSetId": "x"})
    assert created["version"] == 1
    assert created["createdAt"] != "2000-01-01T00:00:00.000Z"
    assert "zoneSet" not in created   # собирает сервер — позже


# ------------------------------------------------------------------ PATCH /maps/{id}

def test_patch_name_only(maps):
    _, created = maps.create(pgmDataUrl=IMAGE)
    code, updated = maps.patch(created["id"], {"version": 1, "name": "Renamed"})

    assert code == 200
    assert updated["version"] == 2
    assert updated["name"] == "Renamed"
    assert updated["waypoints"] == created["waypoints"]     # не прислали — не изменилось
    assert updated["pgmDataUrl"] == IMAGE
    assert maps.from_database(created["id"])[1] == updated


def test_patch_replaces_graph(maps):
    _, created = maps.create()
    waypoints = [{"id": "a", "u": 1, "v": 1}, {"id": "b", "u": 2, "v": 2}]
    edges = [{"id": "ab", "from": "a", "to": "b"}, {"id": "ba", "from": "b", "to": "a"}]
    code, updated = maps.patch(created["id"], {"version": 1, "waypoints": waypoints, "edges": edges,
                                               "stations": []})
    assert code == 200, updated
    _, stored = maps.from_database(created["id"])
    assert (stored["waypoints"], stored["edges"], stored["stations"]) == (waypoints, edges, [])


def test_patch_image(maps):
    _, created = maps.create(pgmDataUrl=IMAGE)
    maps.patch(created["id"], {"version": 1, "pgmDataUrl": "data:image/png;base64,AAAA"})
    assert maps.from_database(created["id"])[1]["pgmDataUrl"] == "data:image/png;base64,AAAA"
    maps.patch(created["id"], {"version": 2, "pgmDataUrl": None})
    assert maps.from_database(created["id"])[1]["pgmDataUrl"] is None


def test_patch_stale_version(maps):
    """Двое открыли версию 1, первый сохранил — второй получает 409 и текущую версию."""
    _, created = maps.create()
    assert maps.patch(created["id"], {"version": 1, "name": "first"})[0] == 200

    code, body = maps.patch(created["id"], {"version": 1, "name": "second"})
    assert code == 409
    assert body["code"] == "VERSION_CONFLICT"
    assert body["details"]["currentVersion"] == 2
    _, stored = maps.from_database(created["id"])
    assert (stored["name"], stored["version"]) == ("first", 2)


def test_concurrent_patches_one_wins(maps):
    """Пять одновременных PATCH по версии 1: проходит один, остальные — 409 с версией 2."""
    _, created = maps.create()
    results: list[tuple[int, dict]] = []

    def save(n: int) -> None:
        results.append(maps.patch(created["id"], {"version": 1, "name": f"writer-{n}"}))

    threads = [threading.Thread(target=save, args=(n,)) for n in range(5)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    codes = sorted(code for code, _ in results)
    assert codes == [200, 409, 409, 409, 409]
    for code, body in results:
        if code == 409:
            assert body["details"]["currentVersion"] == 2
    assert maps.from_database(created["id"])[1]["version"] == 2


@pytest.mark.parametrize("body, field", [
    ({"name": "x"}, "version"),
    ({"version": "1"}, "version"),
    ({"version": -1}, "version"),
    ({"version": 1, "name": None}, "name"),
    ({"version": 1, "name": ""}, "name"),
])
def test_patch_invalid_fields(maps, body, field):
    _, created = maps.create()
    code, error = maps.patch(created["id"], body)
    assert code == 400
    assert error["code"] == "INVALID_INPUT"
    assert field in error["details"]


def test_patch_invalid_map(maps):
    _, created = maps.create()
    code, body = maps.patch(created["id"], {"version": 1, "edges": [{"id": "e9", "from": "n1", "to": "nope"}]})
    assert code == 400
    assert body["code"] == "INVALID_MAP"
    assert maps.from_database(created["id"])[1]["version"] == 1


def test_patch_cannot_change_id(maps):
    _, created = maps.create()
    code, updated = maps.patch(created["id"], {"version": 1, "id": "other-id"})
    assert code == 200
    assert updated["id"] == created["id"]
    assert maps.from_database("other-id")[0] == 404


def test_patch_unknown_map(maps):
    code, body = maps.patch("test-ctl-map-nope", {"version": 1, "name": "x"})
    assert code == 404
    assert body["code"] == "NOT_FOUND"


# ------------------------------------------------------------------ DELETE /maps/{id}

def test_delete_map(maps):
    _, created = maps.create()
    code, _ = maps.delete(created["id"])
    assert code == 204
    assert maps.from_database(created["id"])[0] == 404
    assert maps.delete(created["id"])[0] == 404


def test_recreate_after_delete(maps):
    _, created = maps.create()
    maps.delete(created["id"])
    code, again = maps.create(id=created["id"], name="again")
    assert code == 201
    assert again["version"] == 1


def test_cannot_delete_map_with_robot(maps, sim_robot, robots):
    """На карте стоит робот (его pose.mapId) — 409. Нужен симулятор: карта — та, на которой он стоит."""
    robots.wait_status(sim_robot, lambda s: s["pose"] is not None, 10, "robot pose")
    if maps.from_database(SIM_MAP)[0] == 200:
        pytest.skip(f"map {SIM_MAP} already exists in fms — not deleting someone else's map")
    code, body = maps.create(id=SIM_MAP)
    assert code == 201, body

    code, body = maps.delete(SIM_MAP)
    assert code == 409
    assert body["code"] == "INVALID_STATE"
    assert sim_robot in body["message"]
    assert maps.from_database(SIM_MAP)[0] == 200


# ------------------------------------------------------------------ последствия сохранения (M-3b)
#
# Карта с отрезанными частями или удаление узла, на котором стоит робот, — 409 CONFIRMATION_REQUIRED
# со списком последствий; с ?force=true сохраняется. Мелкие предупреждения (изолированный узел, петля)
# подтверждения не требуют — test_warnings_do_not_block.

def node(node_id: str, u: int) -> dict:
    return {"id": node_id, "u": u, "v": 20}


def road(a: str, b: str) -> list[dict]:
    return [{"id": f"{a}-{b}", "from": a, "to": b}, {"id": f"{b}-{a}", "from": b, "to": a}]


# n1 ↔ n2 → trap: в тупик заехать можно, выехать нельзя
TRAP = {
    "waypoints": [node("n1", 10), node("n2", 50), node("trap", 90)],
    "edges": road("n1", "n2") + [{"id": "n2-trap", "from": "n2", "to": "trap"}],
    "stations": [],
}


def test_trap_needs_confirmation(maps):
    code, body = maps.create(**TRAP)
    assert code == 409
    assert body["code"] == "CONFIRMATION_REQUIRED"
    assert any("trap" in warning for warning in body["details"]["warnings"])
    assert maps.from_database(maps.ids[-1])[0] == 404     # ничего не сохранено


def test_trap_saved_with_force(maps):
    code, created = maps.create(force=True, **TRAP)
    assert code == 201, created
    assert maps.from_database(created["id"])[0] == 200


def test_island_and_unreachable_station(maps):
    """Основная часть n1 ↔ n2 ↔ n3, отдельно остров i1 ↔ i2. Станция st2 подъезжает только с острова."""
    code, body = maps.create(
        waypoints=[node("n1", 10), node("n2", 30), node("n3", 50), node("i1", 120), node("i2", 140)],
        edges=road("n1", "n2") + road("n2", "n3") + road("i1", "i2"),
        stations=[{"id": "st1", "u": 30, "v": 40, "interactionNodeIds": ["n2"]},
                  {"id": "st2", "u": 130, "v": 40, "interactionNodeIds": ["i1"]}],
    )
    assert code == 409
    warnings = " | ".join(body["details"]["warnings"])
    assert "i1" in warnings and "i2" in warnings
    assert "station st2" in warnings
    assert "station st1" not in warnings


def test_patch_with_trap_needs_confirmation(maps):
    _, created = maps.create()
    code, body = maps.patch(created["id"], {"version": 1, **TRAP})
    assert code == 409
    assert body["code"] == "CONFIRMATION_REQUIRED"
    assert maps.from_database(created["id"])[1]["version"] == 1   # не сохранено

    code, updated = maps.patch(created["id"], {"version": 1, **TRAP}, force=True)
    assert code == 200, updated
    assert updated["version"] == 2


def test_removing_robot_node_needs_confirmation(maps, robots, sim_robot):
    """Робот стоит на n1, n1 убирают из карты — после сохранения он станет NOT_ON_GRAPH. Нужен симулятор."""
    if maps.from_database(SIM_MAP)[0] == 200:
        pytest.skip(f"map {SIM_MAP} already exists in fms — not changing someone else's map")
    code, created = maps.create(id=SIM_MAP)
    assert code == 201, created
    robots.place(sim_robot, {"mapId": SIM_MAP, "nodeId": "n1"}, lambda s: s["lastNodeId"] == "n1")

    without_n1 = {"version": 1, "waypoints": [w for w in created["waypoints"] if w["id"] != "n1"],
                  "edges": [], "stations": []}
    code, body = maps.patch(SIM_MAP, without_n1)
    assert code == 409
    assert any(sim_robot in warning and "n1" in warning for warning in body["details"]["warnings"])
    assert maps.from_database(SIM_MAP)[1]["version"] == 1

    code, updated = maps.patch(SIM_MAP, without_n1, force=True)
    assert code == 200, updated


# ------------------------------------------------------------------ большие карты

def test_big_map_5mb(maps):
    """Картинка на 5 МБ: больше лимитов по умолчанию — тела HTTP (1 МБ) и gRPC (4 МБ)."""
    image = "data:image/png;base64," + "A" * (5 * 1024 * 1024)
    code, created = maps.create(pgmDataUrl=image)
    assert code == 201
    assert created["pgmDataUrl"] == image

    # PATCH читает карту из базы целиком — через канал control → база
    code, updated = maps.patch(created["id"], {"version": 1, "name": "big"})
    assert code == 200
    assert updated["pgmDataUrl"] == image
    assert maps.from_database(created["id"])[1]["pgmDataUrl"] == image
