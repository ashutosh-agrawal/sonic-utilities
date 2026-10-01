import pwd
import subprocess
from unittest import mock

import pytest

from redis_display import client, helper, operations
from redis_display.protocol import ProtocolError, decode_message, encode_message


def test_dispatch_rejects_unknown_operation():
    with pytest.raises(operations.OperationError, match="operation denied"):
        operations.dispatch("raw_redis", {"command": "GET"})


def test_dispatch_rejects_extra_argument():
    with pytest.raises(operations.OperationError, match="unexpected argument"):
        operations.feature_status({"feature_name": "bgp", "field": "password"})


def test_protocol_round_trip():
    message = {"version": 1, "operation": "feature_status", "arguments": {}}
    assert decode_message(encode_message(message, 1024), 1024) == message


def test_protocol_requires_newline():
    with pytest.raises(ProtocolError, match="newline terminated"):
        decode_message(b"{}", 1024)


def test_client_invokes_fixed_sudo_helper():
    response = encode_message({"ok": True, "result": {"rows": []}}, 1024)
    completed = subprocess.CompletedProcess([], 0, stdout=response, stderr=b"")
    with mock.patch("redis_display.client.subprocess.run", return_value=completed) as run:
        assert client.request("feature_status", {"feature_name": "bgp"}) == {"rows": []}

    args, kwargs = run.call_args
    assert args[0] == [
        client.SUDO_PATH,
        "-n",
        "-u",
        client.HELPER_USER,
        "--",
        client.HELPER_PATH,
    ]
    assert decode_message(kwargs["input"], 1024) == {
        "version": 1,
        "operation": "feature_status",
        "arguments": {"feature_name": "bgp"},
    }
    assert kwargs["timeout"] == client.DEFAULT_TIMEOUT


def test_client_rejects_failed_sudo_execution():
    completed = subprocess.CompletedProcess([], 1, stdout=b"", stderr=b"denied")
    with mock.patch("redis_display.client.subprocess.run", return_value=completed):
        with pytest.raises(client.DisplayHelperError, match="execution failed"):
            client.request("feature_status", {})


def test_helper_dispatches_as_redis_identity():
    account = pwd.struct_passwd(("redis", "x", 100, 101, "", "/var/lib/redis", "/usr/sbin/nologin"))
    request = encode_message({
        "version": 1,
        "operation": "feature_status",
        "arguments": {"feature_name": "bgp"},
    }, 1024)
    with mock.patch("redis_display.helper.pwd.getpwnam", return_value=account), \
            mock.patch("redis_display.helper.os.geteuid", return_value=100), \
            mock.patch("redis_display.helper.dispatch", return_value={"rows": []}) as dispatch:
        response = decode_message(helper.execute(request), 1024)

    assert response == {"ok": True, "result": {"rows": []}}
    dispatch.assert_called_once_with("feature_status", {"feature_name": "bgp"})


def test_helper_rejects_unprivileged_execution():
    account = pwd.struct_passwd(("redis", "x", 100, 101, "", "/var/lib/redis", "/usr/sbin/nologin"))
    request = encode_message({
        "version": 1,
        "operation": "feature_status",
        "arguments": {},
    }, 1024)
    with mock.patch("redis_display.helper.pwd.getpwnam", return_value=account), \
            mock.patch("redis_display.helper.os.geteuid", return_value=2000):
        response = decode_message(helper.execute(request), 1024)

    assert response == {
        "ok": False,
        "error": "helper must run through its sudo policy",
    }
