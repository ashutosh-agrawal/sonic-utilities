import pwd
from unittest import mock

import pytest

from redis_display import operations
from redis_display.protocol import ProtocolError, decode_message, encode_message
from redis_display.server import peer_is_authorized


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


def test_peer_authorization_accepts_interactive_login():
    account = pwd.struct_passwd(("operator", "x", 1000, 1000, "", "/home/operator", "/bin/bash"))
    with mock.patch("redis_display.server.pwd.getpwuid", return_value=account):
        assert peer_is_authorized(1000)


def test_peer_authorization_rejects_service_account():
    account = pwd.struct_passwd(("redis", "x", 999, 999, "", "/var/lib/redis", "/usr/sbin/nologin"))
    with mock.patch("redis_display.server.pwd.getpwuid", return_value=account):
        assert not peer_is_authorized(999)


def test_peer_authorization_rejects_nologin_shell():
    account = pwd.struct_passwd(("service", "x", 1001, 1001, "", "/", "/usr/sbin/nologin"))
    with mock.patch("redis_display.server.pwd.getpwuid", return_value=account):
        assert not peer_is_authorized(1001)
