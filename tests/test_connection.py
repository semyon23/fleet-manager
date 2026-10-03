"""connection и factsheet (6.5, 6.10): retained, QoS, обязательное содержимое."""

import uuid

import pytest

from fms_client import FmsClient


@pytest.fixture
def fresh_client(target):
    """Новый подписчик: retained-сообщения он должен получить сразу после подписки."""
    client = FmsClient(target, client_id=f"vda-tests-fresh-{uuid.uuid4().hex[:6]}")
    client.connect()
    yield client
    client.close()


def test_connection_online_retained_qos1(fresh_client):
    msg = fresh_client.wait_message("connection", lambda m: True, timeout=3.0,
                                    what="retained connection message")
    rec = fresh_client.messages("connection")[0]
    assert rec.retained, "connection must be published with retained flag (6.5)"
    assert rec.qos == 1, "connection must be published with QoS 1 (4.1)"
    assert msg["connectionState"] == "ONLINE"


def test_factsheet_retained(fresh_client):
    fs = fresh_client.wait_message("factsheet", lambda m: True, timeout=3.0,
                                   what="retained factsheet")
    assert fresh_client.messages("factsheet")[0].retained, "factsheet must be retained (6.10)"
    for section in ("typeSpecification", "physicalParameters", "protocolLimits",
                    "protocolFeatures", "mobileRobotGeometry", "loadSpecification"):
        assert section in fs, f"factsheet: missing section {section}"


def test_factsheet_declares_mandatory_actions(fresh_client):
    fs = fresh_client.wait_message("factsheet", lambda m: True, timeout=3.0)
    actions = {a["actionType"] for a in fs["protocolFeatures"]["mobileRobotActions"]}
    missing = {"cancelOrder", "startPause", "stopPause"} - actions
    assert not missing, f"every robot shall support {missing} (6.2.3)"


def test_factsheet_state_interval_is_consistent(fresh_client):
    fs = fresh_client.wait_message("factsheet", lambda m: True, timeout=3.0)
    timing = fs["protocolLimits"]["timing"]
    assert 0 < timing["minimumStateInterval"] <= 30
