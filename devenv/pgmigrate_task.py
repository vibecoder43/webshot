#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import yaml

from s6.runtime_databases import resolve_runtime_databases


def _repo_root() -> Path:
    root = Path(__file__).resolve().parents[1]
    if not root.joinpath("devenv.nix").is_file():
        raise RuntimeError(f"failed to locate repo root from {__file__}")
    return root


def _run(cmd: list[str]) -> None:
    proc = subprocess.run(cmd, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed (exit={proc.returncode}): {' '.join(cmd)}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="pgmigrate_task")
    parser.add_argument(
        "--config-vars-source",
        required=True,
        help="Path to webshotd config_vars YAML (must contain pg_*_dsn keys)",
    )
    parser.add_argument("--cmd", required=True, choices=["migrate", "baseline"])
    parser.add_argument("--baseline-version", type=int, default=1)
    args = parser.parse_args(argv)

    repo_root = _repo_root()
    config_vars_path = Path(args.config_vars_source).resolve()
    raw = yaml.safe_load(config_vars_path.read_text(encoding="utf-8")) or {}
    if not isinstance(raw, dict):
        raise RuntimeError(f"config vars file must be a YAML mapping: {config_vars_path}")

    databases = resolve_runtime_databases(
        repo_root=repo_root,
        raw_vars=raw,
        source=config_vars_path,
    )

    if args.cmd == "migrate":
        extra_args = ["-t", "latest", "-vv", "migrate"]
    else:
        extra_args = ["-b", str(args.baseline_version), "baseline"]

    for database in databases:
        _run(["pgmigrate", "-c", database.dsn, "-d", str(database.base_dir), *extra_args])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as e:
        print(f"pgmigrate_task: {e}", file=sys.stderr)
        raise SystemExit(1) from None
