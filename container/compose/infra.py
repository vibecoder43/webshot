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
    from compose_tools.infra import (
        infra_down,
        infra_ready,
        infra_status,
        infra_supervise,
        infra_up,
        infra_watch,
    )

    parser = argparse.ArgumentParser(prog="infra.py")
    parser.add_argument("mode", choices=["dev", "prodlike"])
    parser.add_argument("action", choices=["up", "down", "ready", "status", "watch", "supervise"])
    parser.add_argument("--verbose", action="store_true", help="Show diagnostics for 'ready'")
    args = parser.parse_args()

    compose_dir = repo_root / "container/compose"

    try:
        if args.action == "up":
            infra_up(mode=args.mode, compose_dir=compose_dir, repo_root=repo_root)
            return 0
        if args.action == "down":
            infra_down(mode=args.mode, compose_dir=compose_dir)
            return 0
        if args.action == "ready":
            ok = infra_ready(mode=args.mode, compose_dir=compose_dir, verbose=args.verbose)
            return 0 if ok else 1
        if args.action == "status":
            infra_status(mode=args.mode, compose_dir=compose_dir)
            return 0
        if args.action == "watch":
            infra_watch(mode=args.mode, compose_dir=compose_dir)
            return 0
        if args.action == "supervise":
            infra_supervise(mode=args.mode, compose_dir=compose_dir, repo_root=repo_root)
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
