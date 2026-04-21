#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIRS = {
    "dev": REPO_ROOT / "build" / "webshotd" / "san",
    "prodlike": REPO_ROOT / "build" / "webshotd" / "san",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=sorted(BUILD_DIRS), required=True)
    parser.add_argument("--build-dir")
    parser.add_argument("--fail-fast", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_dir = Path(args.build_dir) if args.build_dir else BUILD_DIRS[args.mode]
    testsuite_dir = build_dir / "test"
    log_dir = build_dir / "Testing" / "Temporary"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_name = "testsuite_fail_fast.log" if args.fail_fast else "testsuite.log"
    log_path = log_dir / log_name
    runner_path = testsuite_dir / "runtests-testsuite-tests"

    command = [
        sys.executable,
        str(runner_path),
        "--service-logs-pretty",
        "-vv",
    ]
    if args.fail_fast:
        command.append("-x")

    with log_path.open("wb") as log_file:
        completed = subprocess.run(
            command,
            cwd=testsuite_dir,
            check=False,
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )

    print(log_path)
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
