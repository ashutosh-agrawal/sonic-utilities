import os
import socket

from .protocol import (
    MAX_REQUEST_BYTES,
    MAX_RESPONSE_BYTES,
    PROTOCOL_VERSION,
    ProtocolError,
    decode_message,
    encode_message,
)


DEFAULT_SOCKET_PATH = "/run/sonic-redis-display/redis-display.sock"
DEFAULT_TIMEOUT = 10


class DisplayHelperError(RuntimeError):
    pass


def request(operation, arguments=None, socket_path=None, timeout=DEFAULT_TIMEOUT):
    path = socket_path or os.environ.get("SONIC_REDIS_DISPLAY_SOCKET", DEFAULT_SOCKET_PATH)
    message = {
        "version": PROTOCOL_VERSION,
        "operation": operation,
        "arguments": arguments or {},
    }

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(timeout)
            connection.connect(path)
            connection.sendall(encode_message(message, MAX_REQUEST_BYTES))
            response = _read_response(connection)
    except (OSError, ProtocolError) as exc:
        raise DisplayHelperError("display service is unavailable") from exc

    if response.get("ok") is not True:
        error = response.get("error")
        if not isinstance(error, str) or not error:
            error = "display request failed"
        raise DisplayHelperError(error)
    if "result" not in response:
        raise DisplayHelperError("invalid display service response")
    return response["result"]


def _read_response(connection):
    response = bytearray()
    while b"\n" not in response:
        chunk = connection.recv(min(65536, MAX_RESPONSE_BYTES + 1 - len(response)))
        if not chunk:
            break
        response.extend(chunk)
        if len(response) > MAX_RESPONSE_BYTES:
            raise ProtocolError("response exceeds size limit")
    return decode_message(bytes(response), MAX_RESPONSE_BYTES)
