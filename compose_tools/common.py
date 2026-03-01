from __future__ import annotations

import os
import shlex
import shutil
import subprocess
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ToolError(Exception):
    message: str
    exit_code: int = 2

    def __str__(self) -> str:  # pragma: no cover - trivial
        return self.message


def format_cmd(cmd: Sequence[str]) -> str:
    return " ".join(shlex.quote(part) for part in cmd)


def die(message: str, *, exit_code: int = 2) -> None:
    raise ToolError(message=message, exit_code=exit_code)


def need_cmd(name: str) -> None:
    if shutil.which(name) is None:
        die(f"Missing required command: {name}", exit_code=2)


def need_env(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        die(f"Missing required env var: {name}", exit_code=2)
    return value


def run(
    cmd: Sequence[str],
    *,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    timeout_sec: float | None = None,
    capture: bool = False,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            list(cmd),
            cwd=str(cwd) if cwd is not None else None,
            env=dict(env) if env is not None else None,
            text=True,
            capture_output=capture,
            check=check,
            timeout=timeout_sec,
        )
    except subprocess.TimeoutExpired as e:
        cmd_str = format_cmd(cmd)
        die(f"Timed out running: {cmd_str} (timeout={timeout_sec}s)", exit_code=1)
        raise AssertionError("unreachable") from e
    except subprocess.CalledProcessError as e:
        cmd_str = format_cmd(cmd)
        stdout = (e.stdout or "").strip()
        stderr = (e.stderr or "").strip()
        detail = ""
        if stdout or stderr:
            detail = "\n" + "\n".join(part for part in [stdout, stderr] if part)
        die(f"Command failed (exit={e.returncode}): {cmd_str}{detail}", exit_code=1)
        raise AssertionError("unreachable") from e


def repo_root_from_file(path: Path, *, marker: str = "devenv.nix") -> Path:
    cur = path.resolve()
    for parent in [cur, *cur.parents]:
        if (parent / marker).is_file():
            return parent
    die(f"Failed to find repo root (expected {marker} in a parent directory)", exit_code=2)
    raise AssertionError("unreachable")


def env_with(env: Mapping[str, str], updates: Mapping[str, str]) -> dict[str, str]:
    merged = dict(env)
    merged.update(updates)
    return merged


def tail_lines(path: Path, *, max_lines: int) -> list[str]:
    if max_lines <= 0:
        return []
    try:
        with path.open("rb") as f:
            f.seek(0, os.SEEK_END)
            end = f.tell()
            block = 8192
            data = b""
            pos = end
            while pos > 0 and data.count(b"\n") <= max_lines:
                pos = max(0, pos - block)
                f.seek(pos)
                data = f.read(end - pos) + data
                end = pos
            text = data.decode("utf-8", errors="replace")
    except FileNotFoundError:
        return []
    lines = text.splitlines()
    return lines[-max_lines:]


def wait_for_pid_exit(pid: int, *, timeout_sec: float) -> bool:
    import time

    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        except PermissionError:
            return False
        time.sleep(0.1)
    return False
