import os
import pwd
import sys

from sonic_py_common import logger

from .operations import OperationError, dispatch
from .protocol import (
    MAX_REQUEST_BYTES,
    MAX_RESPONSE_BYTES,
    PROTOCOL_VERSION,
    ProtocolError,
    decode_message,
    encode_message,
)


AUDIT_LOG = logger.Logger("sonic-redis-display")
TARGET_USER = "redis"


def execute(encoded_request):
    operation = "invalid"
    caller_uid = _sudo_identity("SUDO_UID")
    try:
        _require_execution_identity()
        request = decode_message(encoded_request, MAX_REQUEST_BYTES)
        if request.get("version") != PROTOCOL_VERSION:
            raise ProtocolError("unsupported protocol version")
        if set(request) != {"version", "operation", "arguments"}:
            raise ProtocolError("invalid request fields")
        operation = request["operation"]
        result = dispatch(operation, request["arguments"])
        response = {"ok": True, "result": result}
        AUDIT_LOG.log_notice(
            "caller_uid={} operation={} result=ok".format(caller_uid, operation)
        )
    except (OperationError, ProtocolError) as exc:
        response = {"ok": False, "error": str(exc)}
        AUDIT_LOG.log_warning(
            "caller_uid={} operation={} result=denied".format(caller_uid, operation)
        )
    except Exception:
        response = {"ok": False, "error": "display helper request failed"}
        AUDIT_LOG.log_error(
            "caller_uid={} operation={} result=error".format(caller_uid, operation)
        )

    try:
        return encode_message(response, MAX_RESPONSE_BYTES)
    except ProtocolError:
        fallback = {"ok": False, "error": "display result exceeds size limit"}
        return encode_message(fallback, MAX_RESPONSE_BYTES)


def _require_execution_identity():
    target_uid = pwd.getpwnam(TARGET_USER).pw_uid
    if os.geteuid() not in (0, target_uid):
        raise OperationError("helper must run through its sudo policy")


def _sudo_identity(variable):
    value = os.environ.get(variable, "unknown")
    if value != "unknown" and not value.isdigit():
        return "invalid"
    return value


def main():
    encoded_request = sys.stdin.buffer.readline(MAX_REQUEST_BYTES + 1)
    sys.stdout.buffer.write(execute(encoded_request))


if __name__ == "__main__":
    main()
