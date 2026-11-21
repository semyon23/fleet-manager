"""Журнал намерений control.

Нужен только fleet_database.
Чужие записи не изменяются.

DATABASE_RESTART — необязательная команда перезапуска
только fleet_database.
"""

from concurrent.futures import ThreadPoolExecutor
import json
import os
import shlex
import subprocess
import time
import uuid
import warnings

import grpc
import pytest

import fleet_dispatch_pb2 as dp
from conftest import db, DATABASE_URL


@pytest.fixture
def journal(control):
    prefix = "test-command-" + uuid.uuid4().hex
    robot = prefix + "-robot"

    def make(kind=db.CONTROL_COMMAND_KIND_EXTEND_MISSION):
        mission = prefix + "-mission"

        record = db.ControlCommandRecord(
            command_id=prefix + "-" + uuid.uuid4().hex,
            robot_id=robot,
            mission_id=mission,
            map_id="test-map",
            map_version=1,
            kind=kind,
            recovery_version=1,
            recovery_json=json.dumps({
                "test": "restore",
                "base": ["a", "b"],
            }),
        )

        if kind == db.CONTROL_COMMAND_KIND_EXTEND_MISSION:
            record.move_node_id = "s0"
            record.revision = 1

            request = dp.ExtendMissionRequest(
                mission_id=mission,
                move_node_id="s0",
                revision=1,
                released_nodes=2,
            )

        elif kind == db.CONTROL_COMMAND_KIND_ADD_MISSION:
            request = dp.AddMissionRequest()
            request.mission.mission_id = mission
            request.mission.robot_id = robot

            step = request.mission.nodes.add(id="s0")
            step.move.nodes.add(
                node_id="a",
                map_id="test-map",
                x=0,
                y=0,
            )
            step.move.released_nodes = 1

        else:
            request = dp.CancelMissionRequest(
                mission_id=mission
            )

        record.request_proto = request.SerializeToString()
        return record

    yield control, robot, make

    # Удаляем только строки этого теста по уникальному robot_id.
    # Рабочий RPC не удаляет историю команд.
    try:
        result = subprocess.run(
            [
                "psql",
                DATABASE_URL,
                "-v",
                "ON_ERROR_STOP=1",
                "-c",
                "DELETE FROM control_commands "
                "WHERE robot_id = '" + robot + "'",
                ],
            capture_output=True,
            text=True,
            timeout=10,
            env={**os.environ, "LC_ALL": "C.UTF-8"},
        )

        if result.returncode:
            warnings.warn(
                "test command cleanup failed; "
                "test robot id: " + robot
            )

    except (OSError, subprocess.TimeoutExpired):
        warnings.warn(
            "psql cleanup unavailable; "
            "test robot id: " + robot
        )


def put(control, record):
    return control.PutControlCommand(
        db.PutControlCommandRequest(command=record),
        timeout=5,
    )


def entries(control, robot, include=False):
    response = control.ListControlCommands(
        db.ListControlCommandsRequest(
            robot_id=robot,
            include_resolved=include,
        ),
        timeout=5,
    )
    return list(response.commands)


def test_exact_round_trip(journal):
    control, robot, make = journal
    record = make()

    assert not put(control, record).resolved

    got = entries(control, robot)
    assert len(got) == 1
    assert got[0].command == record
    assert not got[0].resolved


@pytest.mark.parametrize(
    "kind",
    [
        db.CONTROL_COMMAND_KIND_ADD_MISSION,
        db.CONTROL_COMMAND_KIND_EXTEND_MISSION,
        db.CONTROL_COMMAND_KIND_CANCEL_MISSION,
    ],
)
def test_command_kinds(journal, kind):
    control, robot, make = journal
    record = make(kind)

    put(control, record)

    assert (
            entries(control, robot)[0].command.request_proto
            == record.request_proto
    )


def test_repeated_put_and_concurrent_retries_do_not_duplicate(
        journal,
):
    control, robot, make = journal
    record = make()

    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(
            pool.map(
                lambda _: put(control, record),
                range(8),
            )
        )

    assert not any(r.resolved for r in results)
    assert len(entries(control, robot)) == 1


def test_same_id_cannot_change_payload_or_recovery(journal):
    control, robot, make = journal
    record = make()
    put(control, record)

    changed = db.ControlCommandRecord()
    changed.CopyFrom(record)
    changed.recovery_json = '{"other":"state"}'

    with pytest.raises(grpc.RpcError) as error:
        put(control, changed)

    assert (
            error.value.code()
            == grpc.StatusCode.ALREADY_EXISTS
    )
    assert entries(control, robot)[0].command == record


def test_same_logical_revision_cannot_get_another_id(journal):
    control, robot, make = journal
    record = make()
    put(control, record)

    changed = db.ControlCommandRecord()
    changed.CopyFrom(record)
    changed.command_id += "-other"

    with pytest.raises(grpc.RpcError) as error:
        put(control, changed)

    assert (
            error.value.code()
            == grpc.StatusCode.ALREADY_EXISTS
    )
    assert len(entries(control, robot)) == 1


def test_resolved_record_cannot_be_reactivated(journal):
    control, robot, make = journal
    record = make()
    put(control, record)

    request = db.ResolveControlCommandRequest(
        command_id=record.command_id
    )

    control.ResolveControlCommand(request, timeout=5)
    control.ResolveControlCommand(request, timeout=5)

    assert not entries(control, robot)

    # Повтор Put не возвращает запись в незавершённые.
    assert put(control, record).resolved

    all_records = entries(control, robot, True)
    assert len(all_records) == 1
    assert all_records[0].resolved


def test_resolution_of_unknown_id(journal):
    control, robot, make = journal

    with pytest.raises(grpc.RpcError) as error:
        control.ResolveControlCommand(
            db.ResolveControlCommandRequest(
                command_id=make().command_id
            ),
            timeout=5,
        )

    assert (
            error.value.code()
            == grpc.StatusCode.NOT_FOUND
    )


@pytest.mark.parametrize(
    "field,value",
    [
        ("command_id", ""),
        ("robot_id", ""),
        ("map_version", 0),
        ("recovery_version", 2),
        ("recovery_json", "[]"),
        ("revision", 0),
        ("request_proto", b"invalid protobuf"),
    ],
)
def test_invalid_record_is_not_saved(journal, field, value):
    control, robot, make = journal
    record = make()
    setattr(record, field, value)

    with pytest.raises(grpc.RpcError) as error:
        put(control, record)

    assert (
            error.value.code()
            == grpc.StatusCode.INVALID_ARGUMENT
    )
    assert not entries(control, robot, True)


def test_records_survive_database_service_restart(journal):
    command = os.environ.get("DATABASE_RESTART")
    if not command:
        pytest.skip(
            "set DATABASE_RESTART to restart fleet_database"
        )

    control, robot, make = journal
    record = make()
    put(control, record)

    subprocess.run(
        shlex.split(command),
        check=True,
        timeout=30,
    )

    deadline = time.monotonic() + 15

    while True:
        try:
            got = entries(control, robot)
            break
        except grpc.RpcError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.2)

    assert len(got) == 1
    assert got[0].command == record


def test_distinct_revisions_are_returned_in_write_order(
        journal,
):
    control, robot, make = journal

    first = make()
    put(control, first)

    second = make()
    second.revision = 2

    request = dp.ExtendMissionRequest()
    request.ParseFromString(second.request_proto)
    request.revision = 2
    request.released_nodes = 3

    second.request_proto = request.SerializeToString()
    put(control, second)

    assert [
               r.command.command_id
               for r in entries(control, robot)
           ] == [first.command_id, second.command_id]

    assert not entries(control, robot + "-missing")


def test_protobuf_bytes_with_unknown_fields_are_preserved(
        journal,
):
    control, robot, make = journal
    record = make()

    # Допустимое неизвестное protobuf-поле 100, varint=1.
    record.request_proto += b"\xa0\x06\x01"

    put(control, record)

    assert (
            entries(control, robot)[0].command.request_proto
            == record.request_proto
    )