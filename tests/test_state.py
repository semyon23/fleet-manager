"""state и visualization (6.6, 6.7): содержимое и частота."""

import os
import time

import pytest

from fms_client import is_idle


def test_state_basic_content(helper):
    s = helper.state()
    assert s["operatingMode"] in ("AUTOMATIC", "SEMIAUTOMATIC")
    pos = s["mobileRobotPosition"]
    assert pos["localized"] is True
    assert pos["mapId"], "mobileRobotPosition.mapId is empty"
    assert -3.15 <= pos["theta"] <= 3.15, "theta must be in [-Pi, Pi]"
    assert 0.0 <= s["powerSupply"]["stateOfCharge"] <= 100.0
    assert isinstance(s["driving"], bool)


def test_state_reports_current_map_enabled(helper):
    s = helper.state()
    if "maps" not in s:
        pytest.skip("robot does not report maps")
    current = s["mobileRobotPosition"]["mapId"]
    enabled = [m["mapId"] for m in s["maps"] if m.get("mapStatus") == "ENABLED"]
    assert current in enabled, f"current map {current} is not ENABLED in state.maps {enabled}"


def test_visualization_published(fms):
    since = fms.mark("visualization")
    try:
        vis = fms.wait_message("visualization", lambda m: True, timeout=3.0, since=since)
    except TimeoutError:
        pytest.skip("robot does not publish visualization (optional topic)")
    assert "mobileRobotPosition" in vis


def test_state_has_no_errors_when_idle(robot):
    s = robot.state()
    assert is_idle(s)
    assert not s["errors"], f"unexpected errors: {s['errors']}"


@pytest.mark.skipif(os.environ.get("VDA_RUN_SLOW") != "1", reason="slow: set VDA_RUN_SLOW=1")
def test_state_sent_at_least_every_30s(robot, fms):
    """Без событий state всё равно уходит не реже раза в 30 с (6.6)."""
    robot.wait_idle(5)
    since = fms.mark()
    start = time.monotonic()
    fms.wait_state(lambda s: True, timeout=35.0, since=since, what="periodic state")
    assert time.monotonic() - start <= 32.0
