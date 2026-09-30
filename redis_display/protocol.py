import json


PROTOCOL_VERSION = 1
MAX_REQUEST_BYTES = 64 * 1024
MAX_RESPONSE_BYTES = 8 * 1024 * 1024


class ProtocolError(ValueError):
    pass


def encode_message(message, limit):
    encoded = (json.dumps(message, separators=(",", ":")) + "\n").encode("utf-8")
    if len(encoded) > limit:
        raise ProtocolError("message exceeds size limit")
    return encoded


def decode_message(encoded, limit):
    if len(encoded) > limit:
        raise ProtocolError("message exceeds size limit")
    if not encoded.endswith(b"\n"):
        raise ProtocolError("message is not newline terminated")
    try:
        message = json.loads(encoded)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError("invalid JSON request") from exc
    if not isinstance(message, dict):
        raise ProtocolError("message must be an object")
    return message
