#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def _bootstrap_repo_root() -> Path:
    root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(root))
    return root


def main() -> int:
    repo_root = _bootstrap_repo_root()

    from compose_tools.common import ToolError
    from compose_tools.webshotd import webshotd_logs, webshotd_start, webshotd_status, webshotd_stop

    parser = argparse.ArgumentParser(prog="webshotd.py")
    parser.add_argument("mode", choices=["dev", "prodlike"])
    parser.add_argument("action", choices=["start", "stop", "status", "logs"])
    args = parser.parse_args()

    try:
        if args.action == "start":
            webshotd_start(mode=args.mode, repo_root=repo_root)
            return 0
        if args.action == "stop":
            webshotd_stop(mode=args.mode)
            return 0
        if args.action == "status":
            webshotd_status(mode=args.mode)
            return 0
        if args.action == "logs":
            webshotd_logs(mode=args.mode)
            return 0
        raise AssertionError("unreachable")
    except ToolError as e:
        if e.message:
            print(e.message, file=sys.stderr)
        return e.exit_code
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
