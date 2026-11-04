"""Карты (M-1): ControlStorage — то, что пишет control, и REST для UI (docs/API_CONTRACT.md, раздел 6).

Карты тесты пишут через gRPC вместо control, читают — через gRPC и REST.
"""

from __future__ import annotations

import json
import threading
import time

import grpc
import pytest

import fleet_database_pb2 as db
from conftest import Maps, error_code
from test_rest import ISO_MS, get

IMAGE = "data:image/png;base64,iVBORw0KGgo="


def body(record: db.MapRecord) -> dict:
    """body_json как объект: JSONB в базе меняет порядок ключей и пробелы, но не содержимое."""
    return json.loads(record.body_json)


def millis(timestamp) -> int:
    return timestamp.seconds * 1000 + timestamp.nanos // 1_000_000


# ------------------------------------------------------------------ CreateMap

def test_create_map(maps):
    created = maps.create(name="Склад «A»", image=IMAGE)

    assert created.version == 1
    assert created.name == "Склад «A»"
    assert body(created) == Maps.BODY
    assert millis(created.created_at) > 0
    assert created.updated_at == created.created_at


def test_create_ignores_version(maps):
    created = maps.create(version=7)
    assert created.version == 1


def test_create_taken_id(maps):
    created = maps.create()
    request = db.CreateMapRequest(map=db.MapRecord(map_id=created.map_id, name="other", body_json="{}"))
    assert error_code(maps.control.CreateMap, request) == grpc.StatusCode.ALREADY_EXISTS
    assert maps.get(created.map_id).name == "Test map"   # первая карта не тронута


@pytest.mark.parametrize("fields", [{"map_id": ""}, {"body_json": ""}])
def test_create_required_fields(maps, fields):
    request = db.CreateMapRequest(map=maps.record(**fields))
    assert error_code(maps.control.CreateMap, request) == grpc.StatusCode.INVALID_ARGUMENT


def test_create_body_not_json(maps):
    request = db.CreateMapRequest(map=maps.record(body_json="{not json"))
    assert error_code(maps.control.CreateMap, request) == grpc.StatusCode.INVALID_ARGUMENT


# ------------------------------------------------------------------ GetMap / ListMaps

def test_get_map_with_image(maps):
    created = maps.create(image=IMAGE)
    got = maps.get(created.map_id)
    assert got.image == IMAGE
    assert body(got) == Maps.BODY
    assert got.version == 1


def test_get_unknown_map(maps):
    request = db.GetMapRequest(map_id="test-map-nope")
    assert error_code(maps.control.GetMap, request) == grpc.StatusCode.NOT_FOUND


def test_list_maps_has_graph_but_no_image(maps):
    created = maps.create(image=IMAGE)
    [listed] = maps.listed()
    assert listed.map_id == created.map_id
    assert body(listed) == Maps.BODY        # control строит графы всех карт из списка
    assert listed.image == ""


def test_list_maps_sorted(maps):
    for _ in range(3):
        maps.create()
    ids = [m.map_id for m in maps.listed()]
    assert len(ids) == 3
    assert ids == sorted(ids)


# ------------------------------------------------------------------ UpdateMap

def test_update_map(maps):
    created = maps.create(image=IMAGE)
    time.sleep(0.01)
    new_body = dict(Maps.BODY, waypoints=[{"id": "n9", "u": 1, "v": 2}], edges=[])
    updated = maps.update(created.map_id, 1, name="Renamed", body_json=json.dumps(new_body))

    assert updated.version == 2
    assert updated.name == "Renamed"
    assert body(updated) == new_body
    assert updated.created_at == created.created_at
    assert millis(updated.updated_at) > millis(created.updated_at)
    assert maps.get(created.map_id).image == IMAGE   # new_image не задан — картинка прежняя


def test_update_replaces_image(maps):
    created = maps.create(image=IMAGE)
    maps.update(created.map_id, 1, new_image="data:image/png;base64,AAAA")
    assert maps.get(created.map_id).image == "data:image/png;base64,AAAA"


def test_update_clears_image(maps):
    """new_image задан, но пустой — это «убрать картинку», а не «не менять»."""
    created = maps.create(image=IMAGE)
    maps.update(created.map_id, 1, new_image="")
    assert maps.get(created.map_id).image == ""


def test_update_version_grows_each_save(maps):
    created = maps.create()
    for expected in range(1, 4):
        assert maps.update(created.map_id, expected).version == expected + 1


def test_update_stale_version(maps):
    """Сохранили версию 2 — правка, сделанная по версии 1, не применяется."""
    created = maps.create()
    maps.update(created.map_id, 1, name="first")

    request = db.UpdateMapRequest(map=db.MapRecord(map_id=created.map_id, name="second", body_json="{}"),
                                  expected_version=1)
    assert error_code(maps.control.UpdateMap, request) == grpc.StatusCode.ABORTED

    got = maps.get(created.map_id)
    assert (got.name, got.version) == ("first", 2)


def test_update_unknown_map(maps):
    request = db.UpdateMapRequest(map=db.MapRecord(map_id="test-map-nope", name="x", body_json="{}"),
                                  expected_version=1)
    assert error_code(maps.control.UpdateMap, request) == grpc.StatusCode.NOT_FOUND


def test_update_body_not_json(maps):
    created = maps.create()
    request = db.UpdateMapRequest(map=db.MapRecord(map_id=created.map_id, name="x", body_json="{not json"),
                                  expected_version=1)
    assert error_code(maps.control.UpdateMap, request) == grpc.StatusCode.INVALID_ARGUMENT
    assert maps.get(created.map_id).version == 1


def test_concurrent_updates_one_wins(maps):
    """Десять одновременных сохранений по одной версии: применяется ровно одно, остальные — ABORTED."""
    created = maps.create()
    results: list[str] = []

    def save(n: int) -> None:
        try:
            maps.update(created.map_id, 1, name=f"writer-{n}")
            results.append("ok")
        except grpc.RpcError as e:
            results.append(e.code().name)

    threads = [threading.Thread(target=save, args=(n,)) for n in range(10)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert results.count("ok") == 1
    assert results.count("ABORTED") == 9
    assert maps.get(created.map_id).version == 2


# ------------------------------------------------------------------ DeleteMap

def test_delete_map(maps):
    created = maps.create()
    maps.delete(created.map_id)
    assert error_code(maps.control.GetMap, db.GetMapRequest(map_id=created.map_id)) == grpc.StatusCode.NOT_FOUND
    assert maps.listed() == []


def test_delete_unknown_map(maps):
    request = db.DeleteMapRequest(map_id="test-map-nope")
    assert error_code(maps.control.DeleteMap, request) == grpc.StatusCode.NOT_FOUND


def test_recreate_after_delete(maps):
    created = maps.create()
    maps.delete(created.map_id)
    again = maps.control.CreateMap(db.CreateMapRequest(
        map=db.MapRecord(map_id=created.map_id, name="again", body_json="{}"))).map
    assert (again.name, again.version) == ("again", 1)


# ------------------------------------------------------------------ большие карты

def test_big_map_5mb(maps):
    """Картинка на 5 МБ — больше лимита gRPC по умолчанию (4 МБ): проходит туда и обратно."""
    image = "data:image/png;base64," + "A" * (5 * 1024 * 1024)
    created = maps.create(image=image)
    assert maps.get(created.map_id).image == image


def test_map_over_server_limit(maps):
    """Больше 32 МБ сервер не принимает (SetMaxReceiveMessageSize в GrpcServer)."""
    request = db.CreateMapRequest(map=maps.record(image="A" * (40 * 1024 * 1024)))
    assert error_code(maps.control.CreateMap, request) == grpc.StatusCode.RESOURCE_EXHAUSTED


# ------------------------------------------------------------------ REST для UI

LIST_FIELDS = {"id", "name", "version", "createdAt", "updatedAt",
               "width", "height", "meta", "stations", "assignedRobots"}


def rest_listed(maps) -> dict:
    status, response = get("/api/fms/maps")
    assert status == 200
    return {m["id"]: m for m in response if m["id"] in maps.ids}


def test_rest_list_only_metadata(maps):
    """Список — без графа и картинки (контракт, раздел 6.2)."""
    created = maps.create(name="Склад", image=IMAGE)
    item = rest_listed(maps)[created.map_id]

    assert set(item) == LIST_FIELDS
    assert item["name"] == "Склад"
    assert item["version"] == 1
    assert item["meta"] == Maps.BODY["meta"]
    assert item["stations"] == Maps.BODY["stations"]
    assert ISO_MS.match(item["createdAt"]) and ISO_MS.match(item["updatedAt"])


def test_rest_get_full_map(maps):
    created = maps.create(image=IMAGE)
    status, item = get(f"/api/fms/maps/{created.map_id}")

    assert status == 200
    assert set(item) == LIST_FIELDS | {"waypoints", "edges", "zones", "pgmDataUrl"}
    assert item["id"] == created.map_id
    assert item["waypoints"] == Maps.BODY["waypoints"]
    assert item["edges"] == Maps.BODY["edges"]
    assert item["pgmDataUrl"] == IMAGE


def test_rest_map_without_image(maps):
    created = maps.create()
    _, item = get(f"/api/fms/maps/{created.map_id}")
    assert item["pgmDataUrl"] is None


def test_rest_shows_new_version(maps):
    created = maps.create()
    maps.update(created.map_id, 1, name="v2")
    _, item = get(f"/api/fms/maps/{created.map_id}")
    assert (item["name"], item["version"]) == ("v2", 2)
    assert rest_listed(maps)[created.map_id]["version"] == 2


def test_rest_unknown_map(maps):
    status, error = get("/api/fms/maps/test-map-nope")
    assert status == 404
    assert error["code"] == "NOT_FOUND"


def test_rest_deleted_map_gone(maps):
    created = maps.create()
    maps.delete(created.map_id)
    assert created.map_id not in rest_listed(maps)
    assert get(f"/api/fms/maps/{created.map_id}")[0] == 404
