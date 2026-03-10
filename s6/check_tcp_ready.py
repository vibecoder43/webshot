#!/usr/bin/env python3
from __future__ import annotations

import socket
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: python -m s6.check_tcp_ready <host> <port>", file=sys.stderr)
        return 2

    try:
        port = int(sys.argv[2])
    except ValueError:
        return 2

    try:
        with socket.create_connection((sys.argv[1], port), timeout=1):
            return 0
    except OSError:
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
