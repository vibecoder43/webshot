#!/usr/bin/env python3
from __future__ import annotations

import sys
import urllib.error
import urllib.request


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python -m s6.check_webshotd_ready <url>", file=sys.stderr)
        return 2

    try:
        with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
            response.read()
        return 0
    except (OSError, urllib.error.URLError):
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
