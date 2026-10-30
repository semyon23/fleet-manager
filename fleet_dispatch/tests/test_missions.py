"""AddMission / CancelMission и стрим статусов миссий (миссии из одного шага)."""

from __future__ import annotations

import grpc
import pytest

import fleet_dispatch_pb2 as dpb
import fms_types_pb2 as fms
from conftest import ORIGIN
from dispatch_client import (action_mission, is_final, mission_of, mission_status, move_mission, new_id, node,
                             robot_status, sim_action, step_action, step_move, step_wait, unknown_action)


def wait_final(robot, missions, mission_id: str, meters: float = 0.0):
    update = missions.wait(mission_status(mission_id, is_final), robot.settings.drive_timeout(meters),
                           what=f"mission {mission_id} finished")
    return update.status


def expect_error(code, call, *args):
    with pytest.raises(grpc.RpcError) as error:
        call(*args)
    assert error.value.code() == code, error.value.details()
    return error.value


# ------------------------------------------------------------------ выполнение

def test_move_mission_completes(robot):
    missions = robot.mission_updates()
    robots = robot.robot_updates()
    mission_id = new_id("move")
    mission = move_mission(mission_id, robot.robot_id, [
        node(ORIGIN, 0.0),
        node("a", 4.0, actions=[sim_action(1.0)]),
        node("b", 8.0),
    ])

    response = robot.add_mission(mission)
    assert response.status.state == fms.MISSION_STATE_RUNNING
    step = response.status.steps[0]
    assert step.state == fms.STEP_STATE_RUNNING
    assert [a.action_id for a in step.actions] == [f"{mission_id}-s1-n1-0"], "dispatch generates action ids"
    assert step.move.total_nodes == 3

    status = wait_final(robot, missions, mission_id, meters=8)
    assert status.state == fms.MISSION_STATE_COMPLETED, status
    step = status.steps[0]
    assert step.state == fms.STEP_STATE_COMPLETED
    assert step.move.last_node_index == 2 and step.move.last_node_id == "b"
    assert step.actions[0].status == fms.ACTION_STATUS_FINISHED
    assert status.HasField("finished_at")
    assert not robot.mqtt.latest("state")["driving"], "mission COMPLETED while the robot was still driving"
    # прогресс шёл по узлам: 0 → 1 → 2, без скачков назад
    indexes = [u.status.steps[0].move.last_node_index for u in missions.items()
               if u.HasField("status") and u.status.mission_id == mission_id]
    assert indexes == sorted(indexes) and 1 in indexes, f"progress by nodes: {indexes}"

    # робот свободен и стоит в конце маршрута
    robots.wait(robot_status(robot.robot_id, lambda s: s.mission_id == "" and s.last_node_id == "b"),
                robot.settings.t(5), what="robot free at node b")


def test_action_mission_completes(robot):
    missions = robot.mission_updates()
    mission_id = new_id("action")
    robot.add_mission(action_mission(mission_id, robot.robot_id, [sim_action(2.0)]))

    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_COMPLETED, status
    assert status.steps[0].actions[0].status == fms.ACTION_STATUS_FINISHED


def test_robot_rejection_fails_mission(robot):
    """Робот отклоняет order (неизвестная карта) — миссия FAILED с ORDER_REJECTED."""
    missions = robot.mission_updates()
    mission_id = new_id("rejected")
    robot.add_mission(move_mission(mission_id, robot.robot_id, [
        node(ORIGIN, 0.0), node("x", 3.0, map_id="no_such_map")]))

    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_FAILED, status
    assert status.error.code == fms.MISSION_ERROR_CODE_ORDER_REJECTED
    assert status.error.message, "error message should explain the rejection"


def test_finished_mission_stays_in_snapshot(robot):
    missions = robot.mission_updates()
    mission_id = new_id("snapshot")
    robot.add_mission(action_mission(mission_id, robot.robot_id, [sim_action(1.0)]))
    wait_final(robot, missions, mission_id)

    # control, подключившийся позже, должен узнать итог из снимка
    later = robot.mission_updates()
    first = later.wait(lambda u: True, robot.settings.t(3), what="first message")
    assert first.HasField("snapshot")
    states = {m.mission_id: m.state for m in first.snapshot.missions}
    assert states.get(mission_id) == fms.MISSION_STATE_COMPLETED


# ------------------------------------------------------------------ проверки AddMission

def test_busy_robot_rejects_second_mission(robot):
    mission_id = new_id("busy")
    robot.add_mission(move_mission(mission_id, robot.robot_id, [node(ORIGIN, 0.0), node("far", 15.0)]))

    other = action_mission(new_id("other"), robot.robot_id, [sim_action(1.0)])
    expect_error(grpc.StatusCode.FAILED_PRECONDITION, robot.add_mission, other)

    # та же миссия ещё раз — повтор запроса, а не ошибка
    again = robot.add_mission(move_mission(mission_id, robot.robot_id, [node(ORIGIN, 0.0), node("far", 15.0)]))
    assert again.status.mission_id == mission_id


def test_route_must_start_where_robot_is(robot):
    mission = move_mission(new_id("wrong-start"), robot.robot_id, [node("elsewhere", 1.0), node("y", 5.0)])
    error = expect_error(grpc.StatusCode.FAILED_PRECONDITION, robot.add_mission, mission)
    assert ORIGIN in error.details()


def notify_step(robot):
    mission = fms.Mission(mission_id=new_id("notify"), robot_id=robot.robot_id)
    mission.nodes.add(id="s1", notify=fms.NotifyStep(topic="loaded"))
    return mission


def nested_step(robot):
    mission = action_mission(new_id("nested"), robot.robot_id, [sim_action(1.0)])
    mission.nodes[0].parent_id = "root"
    return mission


def duplicate_step_ids(robot):
    return mission_of(new_id("dup"), robot.robot_id, [step_action("s1", [sim_action(1.0)]),
                                                      step_action("s1", [sim_action(1.0)])])


def wait_step(robot):
    mission = fms.Mission(mission_id=new_id("wait"), robot_id=robot.robot_id)
    mission.nodes.add(id="s1", wait=fms.WaitStep(signal="door"))
    return mission


def missing_edge(robot):
    mission = move_mission(new_id("edges"), robot.robot_id, [node(ORIGIN, 0.0), node("y", 5.0)])
    del mission.nodes[0].move.edges[:]
    return mission


def no_blocking_type(robot):
    return action_mission(new_id("blocking"), robot.robot_id, [fms.Action(action_type="simulatedAction")])


def no_mission_id(robot):
    return action_mission("", robot.robot_id, [sim_action(1.0)])


@pytest.mark.parametrize("build, code", [
    (notify_step, grpc.StatusCode.UNIMPLEMENTED),
    (nested_step, grpc.StatusCode.UNIMPLEMENTED),
    (wait_step, grpc.StatusCode.UNIMPLEMENTED),          # ожидание сигнала — позже
    (duplicate_step_ids, grpc.StatusCode.INVALID_ARGUMENT),
    (missing_edge, grpc.StatusCode.INVALID_ARGUMENT),
    (no_blocking_type, grpc.StatusCode.INVALID_ARGUMENT),
    (no_mission_id, grpc.StatusCode.INVALID_ARGUMENT),
])
def test_invalid_mission_is_rejected(robot, build, code):
    expect_error(code, robot.add_mission, build(robot))


def test_same_mission_on_another_robot(robot, second_robot):
    mission_id = new_id("shared")
    robot.add_mission(move_mission(mission_id, robot.robot_id, [node(ORIGIN, 0.0), node("far", 15.0)]))

    copy = action_mission(mission_id, second_robot.robot_id, [sim_action(1.0)])
    expect_error(grpc.StatusCode.ALREADY_EXISTS, second_robot.add_mission, copy)


# ------------------------------------------------------------------ отмена и удаление

def test_cancel_mission(robot):
    missions = robot.mission_updates()
    robots = robot.robot_updates()
    mission_id = new_id("cancel")
    robot.add_mission(move_mission(mission_id, robot.robot_id, [node(ORIGIN, 0.0), node("far", 15.0)]))
    robots.wait(robot_status(robot.robot_id, lambda s: s.driving), robot.settings.t(5), what="robot driving")

    robot.cancel_mission(mission_id)
    missions.wait(mission_status(mission_id, lambda s: s.state == fms.MISSION_STATE_CANCELING),
                  robot.settings.t(3), what="CANCELING")
    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_CANCELED, status
    assert status.steps[0].state == fms.STEP_STATE_CANCELED

    robot.wait_idle(robot.settings.t(5))   # робот на самом деле остановился и бросил order
    expect_error(grpc.StatusCode.NOT_FOUND, robot.cancel_mission, mission_id)


def test_cancel_unknown_mission(dispatch):
    with pytest.raises(grpc.RpcError) as error:
        dispatch.CancelMission(dpb.CancelMissionRequest(mission_id="no-such-mission"))
    assert error.value.code() == grpc.StatusCode.NOT_FOUND


def test_remove_robot_fails_its_mission(robot):
    missions = robot.mission_updates()
    robots = robot.robot_updates()
    mission_id = new_id("removed")
    robot.add_mission(move_mission(mission_id, robot.robot_id, [node(ORIGIN, 0.0), node("far", 15.0)]))
    robots.wait(robot_status(robot.robot_id, lambda s: s.driving), robot.settings.t(5), what="robot driving")

    robot.remove()
    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_FAILED, status
    assert status.error.code == fms.MISSION_ERROR_CODE_ROBOT_REMOVED

    # dispatch робота больше не ведёт, а симулятор всё ещё едет — остановить напрямую
    from action_helpers import send_instant_actions
    send_instant_actions(robot.mqtt, [{"actionId": new_id("cancelOrder"), "actionType": "cancelOrder",
                                       "blockingType": "NONE"}])
    robot.wait_idle(robot.settings.t(5))


# ------------------------------------------------------------------ миссии из нескольких шагов

def test_multi_step_mission_completes(robot):
    missions = robot.mission_updates()
    mission_id = new_id("multi")
    mission = mission_of(mission_id, robot.robot_id, [
        step_move("go", [node(ORIGIN, 0.0), node("a", 4.0)]),
        step_action("work", [sim_action(1.0)]),
        step_wait("rest", 1.0),
        step_move("back", [node("a", 4.0), node("b", 6.0)]),
    ])

    response = robot.add_mission(mission)
    assert [s.state for s in response.status.steps] == [
        fms.STEP_STATE_RUNNING, fms.STEP_STATE_PENDING, fms.STEP_STATE_PENDING, fms.STEP_STATE_PENDING]

    status = wait_final(robot, missions, mission_id, meters=6)
    assert status.state == fms.MISSION_STATE_COMPLETED, status
    assert all(s.state == fms.STEP_STATE_COMPLETED for s in status.steps), status

    # шаги шли строго по порядку
    current = [u.status.current_node_id for u in missions.items()
               if u.HasField("status") and u.status.mission_id == mission_id and u.status.current_node_id]
    order = [c for i, c in enumerate(current) if i == 0 or current[i - 1] != c]
    assert order == ["go", "work", "rest", "back"], order


def test_failed_step_can_be_skipped(robot):
    missions = robot.mission_updates()
    mission_id = new_id("skip")
    robot.add_mission(mission_of(mission_id, robot.robot_id, [
        step_action("bad", [unknown_action()], on_failure=fms.FAILURE_ACTION_SKIP),
        step_action("good", [sim_action(1.0)]),
    ]))

    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_COMPLETED, status
    assert status.steps[0].state == fms.STEP_STATE_SKIPPED
    assert status.steps[1].state == fms.STEP_STATE_COMPLETED


def test_failed_step_is_retried_then_fails(robot):
    missions = robot.mission_updates()
    mission_id = new_id("retry")
    robot.add_mission(mission_of(mission_id, robot.robot_id, [
        step_action("bad", [unknown_action()], on_failure=fms.FAILURE_ACTION_RETRY, max_retries=2),
    ]))

    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_FAILED, status
    assert status.error.code == fms.MISSION_ERROR_CODE_ORDER_REJECTED
    assert status.steps[0].attempts == 3, "1 attempt + 2 retries"


def test_step_timeout_fails_mission_and_stops_robot(robot):
    missions = robot.mission_updates()
    mission_id = new_id("timeout")
    robot.add_mission(mission_of(mission_id, robot.robot_id, [
        step_move("far", [node(ORIGIN, 0.0), node("far", 15.0)], timeout=2.0),
    ]))

    status = wait_final(robot, missions, mission_id)
    assert status.state == fms.MISSION_STATE_FAILED, status
    assert status.error.code == fms.MISSION_ERROR_CODE_TIMEOUT
    robot.wait_idle(robot.settings.t(5))   # dispatch остановил робота cancelOrder'ом


def test_route_chain_is_checked(robot):
    """Второй маршрут должен начинаться там, где закончится первый."""
    mission = mission_of(new_id("chain"), robot.robot_id, [
        step_move("go", [node(ORIGIN, 0.0), node("a", 4.0)]),
        step_move("jump", [node("b", 8.0), node("c", 10.0)]),
    ])
    error = expect_error(grpc.StatusCode.FAILED_PRECONDITION, robot.add_mission, mission)
    assert "jump" in error.details()
