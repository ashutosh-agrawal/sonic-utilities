import argparse
import logging
import os
import pwd
import socket
import socketserver
import stat
import struct

from utilities_common.general import load_db_config

from .client import DEFAULT_SOCKET_PATH
from .operations import OperationError, dispatch
from .protocol import (
    MAX_REQUEST_BYTES,
    MAX_RESPONSE_BYTES,
    PROTOCOL_VERSION,
    ProtocolError,
    decode_message,
    encode_message,
)


LOGGER = logging.getLogger("sonic-redis-displayd")
MIN_LOGIN_UID = 1000
REQUEST_TIMEOUT_SECONDS = 10
DENIED_SHELLS = {"/bin/false", "/usr/bin/false", "/sbin/nologin", "/usr/sbin/nologin"}


class DisplayRequestHandler(socketserver.StreamRequestHandler):
    def handle(self):
        self.request.settimeout(REQUEST_TIMEOUT_SECONDS)
        uid, gid = self._peer_credentials()
        operation = "invalid"
        try:
            if not peer_is_authorized(uid):
                raise OperationError("caller is not authorized")
            encoded = self.rfile.readline(MAX_REQUEST_BYTES + 1)
            request = decode_message(encoded, MAX_REQUEST_BYTES)
            if request.get("version") != PROTOCOL_VERSION:
                raise ProtocolError("unsupported protocol version")
            if set(request) != {"version", "operation", "arguments"}:
                raise ProtocolError("invalid request fields")
            operation = request["operation"]
            result = dispatch(operation, request["arguments"])
            response = {"ok": True, "result": result}
            LOGGER.info("uid=%s gid=%s operation=%s result=ok", uid, gid, operation)
        except (OperationError, ProtocolError) as exc:
            response = {"ok": False, "error": str(exc)}
            LOGGER.warning("uid=%s gid=%s operation=%s result=denied", uid, gid, operation)
        except Exception:
            response = {"ok": False, "error": "display service request failed"}
            LOGGER.exception("uid=%s gid=%s operation=%s result=error", uid, gid, operation)

        try:
            try:
                encoded_response = encode_message(response, MAX_RESPONSE_BYTES)
            except ProtocolError:
                fallback = {"ok": False, "error": "display result exceeds size limit"}
                encoded_response = encode_message(fallback, MAX_RESPONSE_BYTES)
            self.wfile.write(encoded_response)
        except (BrokenPipeError, ConnectionResetError):
            LOGGER.info("uid=%s gid=%s operation=%s result=client-disconnected", uid, gid, operation)

    def _peer_credentials(self):
        encoded = self.request.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        _, uid, gid = struct.unpack("3i", encoded)
        return uid, gid


class DisplayServer(socketserver.UnixStreamServer):
    pass


def peer_is_authorized(uid):
    if uid == 0:
        return True
    if uid < MIN_LOGIN_UID or uid == 65534:
        return False
    try:
        account = pwd.getpwuid(uid)
    except KeyError:
        return False
    return account.pw_shell not in DENIED_SHELLS


def serve(socket_path):
    load_db_config()
    _remove_stale_socket(socket_path)
    with DisplayServer(socket_path, DisplayRequestHandler) as server:
        # The listener must be reachable by non-admin login users. Authorization is
        # performed from kernel-provided peer credentials before parsing a request.
        os.chmod(socket_path, 0o666)
        try:
            server.serve_forever()
        finally:
            try:
                os.unlink(socket_path)
            except FileNotFoundError:
                pass


def _remove_stale_socket(socket_path):
    try:
        mode = os.lstat(socket_path).st_mode
    except FileNotFoundError:
        return
    if not stat.S_ISSOCK(mode):
        raise RuntimeError("refusing to replace non-socket path")
    os.unlink(socket_path)


def main():
    parser = argparse.ArgumentParser(description="Serve approved SONiC Redis display reads")
    parser.add_argument("--socket", default=DEFAULT_SOCKET_PATH)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(name)s: %(message)s")
    serve(args.socket)


if __name__ == "__main__":
    main()
