#!/usr/bin/env python3
from __future__ import annotations

import ssl
import sys
import urllib.error
import urllib.request


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print("usage: python -m s6.check_http_ready <url> [--insecure]", file=sys.stderr)
        return 2

    insecure = len(sys.argv) == 3
    if insecure and sys.argv[2] != "--insecure":
        print("usage: python -m s6.check_http_ready <url> [--insecure]", file=sys.stderr)
        return 2

    context = ssl._create_unverified_context() if insecure else None

    try:
        with urllib.request.urlopen(sys.argv[1], timeout=1, context=context) as response:
            response.read()
        return 0
    except (OSError, urllib.error.URLError):
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
