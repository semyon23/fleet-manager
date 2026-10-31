"""DB-4: dispatch пишет статус робота и factsheet в fleet_database.

Фикстура robot (conftest.py) регистрирует робота в базе как test-<serial> до того, как добавить его
в dispatch, — как это делает control, — и удаляет из базы после теста.
"""

from __future__ import annotations

import time
import uuid

import fleet_database_pb2 as db
import fms_types_pb2 as fms
from action_helpers import send_instant_actions
from conftest import ORIGIN
from dispatch_client import robot_status


def wait_saved_status(database, robot_id: str, condition, timeout: float, what: str) -> fms.RobotStatus:
    """Ждать, пока в базе появится статус робота, для которого выполняется condition."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        record = database.GetRobot(db.GetRobotRequest(robot_id=robot_id)).robot
        if record.HasField("status"):
            last = record.status
            if condition(last):
                return last
        time.sleep(0.2)
    raise TimeoutError(f"timeout {timeout:.1f}s waiting for saved status: {what}\nlast saved: {last}")


# ------------------------------------------------------------------ статус

def test_status_is_saved(robot, database, settings):
    status = wait_saved_status(
        database, robot.robot_id,
        lambda s: s.connection == fms.CONNECTION_STATE_ONLINE and s.last_node_id == ORIGIN,
        settings.t(5), "ONLINE on origin")
    assert status.robot_id == robot.robot_id
    assert status.HasField("last_state_at")


def test_important_change_is_saved_at_once(robot, database, settings):
    """Новый узел — важное изменение: в базе сразу, без ожидания 5 с для позы и батареи."""
    robot.initialize_position("db-node", 0.0, 0.0)
    wait_saved_status(database, robot.robot_id, lambda s: s.last_node_id == "db-node",
                      settings.t(3), "last_node_id db-node")   # 3 с — меньше 5 с, через которые пишется поза


def test_robot_removed_from_dispatch_keeps_last_status(robot, database, settings):
    """dispatch не удаляет статус из базы — это делает control вместе с роботом (DeleteRobot)."""
    wait_saved_status(database, robot.robot_id, lambda s: s.connection == fms.CONNECTION_STATE_ONLINE,
                      settings.t(5), "ONLINE")
    robot.remove()
    record = database.GetRobot(db.GetRobotRequest(robot_id=robot.robot_id)).robot
    assert record.status.connection == fms.CONNECTION_STATE_ONLINE


# ------------------------------------------------------------------ factsheet

def test_factsheet_is_in_database_before_stream_event(robot, database, mqtt, settings):
    """Пришёл factsheet — сначала запись в базу, потом новое factsheet_received_at в стриме.
    Увидев событие, control читает factsheet из базы — он должен уже быть там."""
    updates = robot.robot_updates()
    snapshot = updates.wait(lambda u: u.HasField("snapshot"), settings.t(5), what="snapshot").snapshot
    saved = next(r for r in snapshot.robots if r.robot_id == robot.robot_id)
    previous = saved.factsheet_received_at.ToMilliseconds() if saved.HasField("factsheet_received_at") else 0

    since = updates.mark()
    send_instant_actions(mqtt, [{"actionId": f"factsheetRequest-{uuid.uuid4().hex[:8]}",
                                 "actionType": "factsheetRequest", "blockingType": "NONE"}])
    update = updates.wait(
        robot_status(robot.robot_id, lambda s: s.HasField("factsheet_received_at")
                     and s.factsheet_received_at.ToMilliseconds() > previous),
        settings.t(5), since=since, what="new factsheet_received_at")

    factsheet = database.GetFactsheet(db.GetFactsheetRequest(robot_id=robot.robot_id))
    assert factsheet.received_at == update.status.factsheet_received_at
    assert '"typeSpecification"' in factsheet.factsheet_json
