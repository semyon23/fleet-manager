"""RobotRegistry: создание, чтение, изменение и удаление роботов в два шага."""

from __future__ import annotations

import time

import grpc

from conftest import MANUFACTURER, db, error_code, unique

ACTIVE = db.ROBOT_LIFECYCLE_ACTIVE
PENDING_DELETE = db.ROBOT_LIFECYCLE_PENDING_DELETE


# ------------------------------------------------------------------ CreateRobot

def test_create_returns_stored_robot(robots):
    spec = robots.create(name="Робот 1", labels={"zone": "A", "line": "2"})

    assert spec.manufacturer == MANUFACTURER
    assert spec.name == "Робот 1"
    assert dict(spec.labels) == {"zone": "A", "line": "2"}
    assert spec.lifecycle == ACTIVE
    assert spec.created_at.ToMilliseconds() > 0
    assert spec.updated_at == spec.created_at


def test_create_ignores_lifecycle_and_timestamps(robots):
    request = robots.spec(lifecycle=PENDING_DELETE)
    request.created_at.FromMilliseconds(1000)

    spec = robots.registry.CreateRobot(db.CreateRobotRequest(spec=request)).spec

    assert spec.lifecycle == ACTIVE
    assert spec.created_at.ToMilliseconds() > 1000


def test_create_without_labels_and_name(robots):
    spec = robots.create()
    assert spec.name == ""
    assert dict(spec.labels) == {}


def test_labels_and_name_with_special_characters(robots):
    """Кавычки и прочее в метках уходят в базу параметрами и JSON'ом — без поломок."""
    labels = {"note": "a \"b\" 'c' \\ ; DROP TABLE robots; --", "пусто": "", "emoji": "🤖"}
    spec = robots.create(name="O'Brien \"1\"", labels=labels)

    stored = robots.get(spec.robot_id)
    assert stored.name == "O'Brien \"1\""
    assert dict(stored.labels) == labels


def test_create_same_robot_id_fails(robots):
    spec = robots.create()
    again = robots.spec(robot_id=spec.robot_id)
    assert error_code(robots.registry.CreateRobot, db.CreateRobotRequest(spec=again)) == grpc.StatusCode.ALREADY_EXISTS


def test_create_same_physical_robot_under_other_id_fails(robots):
    spec = robots.create()
    other = robots.spec(serial_number=spec.serial_number)
    assert error_code(robots.registry.CreateRobot, db.CreateRobotRequest(spec=other)) == grpc.StatusCode.ALREADY_EXISTS


def test_same_serial_of_other_manufacturer_is_allowed(robots):
    spec = robots.create()
    other = robots.create(manufacturer="other", serial_number=spec.serial_number)
    assert other.serial_number == spec.serial_number


def test_create_requires_id_manufacturer_serial(robots):
    for missing in ("robot_id", "manufacturer", "serial_number"):
        spec = robots.spec(**{missing: ""})
        code = error_code(robots.registry.CreateRobot, db.CreateRobotRequest(spec=spec))
        assert code == grpc.StatusCode.INVALID_ARGUMENT, missing


def test_parallel_creates_of_same_robot_only_one_succeeds(robots):
    """Запросы к базе идут по одному в потоке базы: из одновременных CreateRobot с одним id проходит ровно один."""
    spec = robots.spec()
    calls = [robots.registry.CreateRobot.future(db.CreateRobotRequest(spec=spec)) for _ in range(10)]

    codes = [call.code() for call in calls]   # code() ждёт, пока вызов закончится
    assert codes.count(grpc.StatusCode.OK) == 1
    assert codes.count(grpc.StatusCode.ALREADY_EXISTS) == 9


def test_parallel_creates_of_different_robots_all_succeed(robots):
    calls = [robots.registry.CreateRobot.future(db.CreateRobotRequest(spec=robots.spec())) for _ in range(20)]

    assert [call.code() for call in calls] == [grpc.StatusCode.OK] * 20
    assert len(robots.listed()) == 20


# ------------------------------------------------------------------ GetRobot / ListRobots

def test_get_returns_what_create_returned(robots):
    spec = robots.create(name="r", labels={"zone": "A"})
    response = robots.registry.GetRobot(db.GetRobotRequest(robot_id=spec.robot_id))

    assert response.robot.spec == spec
    assert not response.robot.HasField("status")   # статус пишет dispatch — ещё не реализовано


def test_get_unknown_robot(robots):
    assert error_code(robots.registry.GetRobot, db.GetRobotRequest(robot_id="test-no-such-robot")) \
        == grpc.StatusCode.NOT_FOUND


def test_list_is_sorted_by_robot_id(robots):
    for prefix in ("test-c", "test-a", "test-b"):
        robots.create(robot_id=unique(prefix))
    listed = [spec.robot_id for spec in robots.listed()]
    assert listed == sorted(listed)
    assert len(listed) == 3


# ------------------------------------------------------------------ UpdateRobot

def test_update_changes_only_name_and_labels(robots):
    spec = robots.create(name="old", labels={"zone": "A"})
    time.sleep(0.01)   # чтобы updated_at точно отличался от created_at

    change = db.RobotSpec(robot_id=spec.robot_id, manufacturer="other", serial_number="other",
                          name="new", labels={"zone": "B"}, lifecycle=PENDING_DELETE)
    updated = robots.registry.UpdateRobot(db.UpdateRobotRequest(spec=change)).spec

    assert updated.name == "new"
    assert dict(updated.labels) == {"zone": "B"}
    assert updated.manufacturer == spec.manufacturer
    assert updated.serial_number == spec.serial_number
    assert updated.lifecycle == ACTIVE
    assert updated.created_at == spec.created_at
    assert updated.updated_at.ToMilliseconds() > spec.updated_at.ToMilliseconds()
    assert robots.get(spec.robot_id) == updated


def test_update_replaces_labels_completely(robots):
    """Метки заменяются целиком: каких нет в запросе — удаляются."""
    spec = robots.create(labels={"zone": "A", "line": "2"})
    change = db.RobotSpec(robot_id=spec.robot_id, name=spec.name)
    updated = robots.registry.UpdateRobot(db.UpdateRobotRequest(spec=change)).spec
    assert dict(updated.labels) == {}


def test_update_unknown_robot(robots):
    change = db.RobotSpec(robot_id="test-no-such-robot", name="x")
    assert error_code(robots.registry.UpdateRobot, db.UpdateRobotRequest(spec=change)) == grpc.StatusCode.NOT_FOUND


def test_update_requires_robot_id(robots):
    assert error_code(robots.registry.UpdateRobot, db.UpdateRobotRequest(spec=db.RobotSpec(name="x"))) \
        == grpc.StatusCode.INVALID_ARGUMENT


# ------------------------------------------------------------------ удаление в два шага

def test_delete_without_set_deleting_fails(robots):
    spec = robots.create()
    assert error_code(robots.registry.DeleteRobot, db.DeleteRobotRequest(robot_id=spec.robot_id)) \
        == grpc.StatusCode.FAILED_PRECONDITION
    assert robots.get(spec.robot_id).lifecycle == ACTIVE


def test_pending_delete_robot_is_hidden_from_list_by_default(robots):
    kept = robots.create()
    deleting = robots.create()
    robots.set_deleting(deleting.robot_id)

    assert [s.robot_id for s in robots.listed()] == [kept.robot_id]

    with_deleting = {s.robot_id: s.lifecycle for s in robots.listed(include_deleting=True)}
    assert with_deleting == {kept.robot_id: ACTIVE, deleting.robot_id: PENDING_DELETE}

    assert robots.get(deleting.robot_id).lifecycle == PENDING_DELETE


def test_set_deleting_twice_is_ok(robots):
    spec = robots.create()
    robots.set_deleting(spec.robot_id)
    robots.set_deleting(spec.robot_id)
    assert robots.get(spec.robot_id).lifecycle == PENDING_DELETE


def test_set_deleting_unknown_robot(robots):
    assert error_code(robots.registry.SetRobotDeleting, db.SetRobotDeletingRequest(robot_id="test-no-such-robot")) \
        == grpc.StatusCode.NOT_FOUND


def test_delete_after_set_deleting(robots):
    spec = robots.create()
    robots.set_deleting(spec.robot_id)
    robots.delete(spec.robot_id)

    assert error_code(robots.registry.GetRobot, db.GetRobotRequest(robot_id=spec.robot_id)) \
        == grpc.StatusCode.NOT_FOUND
    assert robots.listed(include_deleting=True) == []
    assert error_code(robots.registry.DeleteRobot, db.DeleteRobotRequest(robot_id=spec.robot_id)) \
        == grpc.StatusCode.NOT_FOUND


def test_deleted_robot_can_be_registered_again(robots):
    """После удаления тот же физический робот (и тот же robot_id) регистрируется заново."""
    spec = robots.create()
    robots.set_deleting(spec.robot_id)
    robots.delete(spec.robot_id)

    again = robots.create(robot_id=spec.robot_id, serial_number=spec.serial_number)
    assert again.lifecycle == ACTIVE
