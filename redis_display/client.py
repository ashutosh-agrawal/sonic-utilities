import subprocess

from .protocol import (
    MAX_REQUEST_BYTES,
    MAX_RESPONSE_BYTES,
    PROTOCOL_VERSION,
    ProtocolError,
    decode_message,
    encode_message,
)


SUDO_PATH = "/usr/bin/sudo"
HELPER_PATH = "/usr/local/bin/sonic-redis-display"
HELPER_USER = "redis"
DEFAULT_TIMEOUT = 10


class DisplayHelperError(RuntimeError):
    pass


def request(operation, arguments=None, timeout=DEFAULT_TIMEOUT):
    message = {
        "version": PROTOCOL_VERSION,
        "operation": operation,
        "arguments": arguments or {},
    }

    try:
        completed = subprocess.run(
            [SUDO_PATH, "-n", "-u", HELPER_USER, "--", HELPER_PATH],
            input=encode_message(message, MAX_REQUEST_BYTES),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
        if completed.returncode != 0:
            raise DisplayHelperError("display helper execution failed")
        response = decode_message(completed.stdout, MAX_RESPONSE_BYTES)
    except (OSError, subprocess.TimeoutExpired, ProtocolError) as exc:
        raise DisplayHelperError("display helper is unavailable") from exc

    if response.get("ok") is not True:
        error = response.get("error")
        if not isinstance(error, str) or not error:
            error = "display request failed"
        raise DisplayHelperError(error)
    if "result" not in response:
        raise DisplayHelperError("invalid display service response")
    return response["result"]
