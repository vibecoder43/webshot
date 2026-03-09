#!/usr/bin/env python3
from __future__ import annotations

import socket
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python -m compose_tools.check_crawlerd_ready <socket-path>", file=sys.stderr)
        return 2

    socket_path = Path(sys.argv[1])
    if not socket_path.is_socket():
        return 1

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(1.0)
            sock.connect(str(socket_path))
            sock.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            return 0 if b"200" in sock.recv(4096) else 1
    except OSError:
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
