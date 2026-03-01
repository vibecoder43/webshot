from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path

from compose_tools.common import ToolError, die, need_cmd, run

_checked_compose = False


@dataclass(frozen=True)
class ContainerState:
    status: str
    health: str
    exit_code: int


def require_podman_compose() -> None:
    global _checked_compose
    if _checked_compose:
        return
    need_cmd("podman")
    proc = run(["podman", "compose", "version"], capture=True, check=False)
    if proc.returncode != 0:
        die("Missing compose support (need 'podman compose').", exit_code=2)
    _checked_compose = True


def compose(args: list[str], *, cwd: Path) -> None:
    require_podman_compose()
    run(["podman", "compose", *args], cwd=cwd)


def podman_inspect_state(name: str) -> ContainerState:
    require_podman_compose()
    out = run(
        [
            "podman",
            "inspect",
            "-f",
            "{{.State.Status}}|{{if .State.Health}}{{.State.Health.Status}}{{end}}"
            "|{{.State.ExitCode}}",
            name,
        ],
        capture=True,
    ).stdout.strip()
    parts = out.split("|")
    if len(parts) != 3:
        die(f"Unexpected podman inspect output for '{name}': {out!r}", exit_code=1)
    status, health, exit_code_raw = parts
    exit_code = 0
    try:
        exit_code = int(exit_code_raw) if exit_code_raw else 0
    except ValueError:
        die(
            f"Unexpected exit code from podman inspect for '{name}': {exit_code_raw!r}", exit_code=1
        )
    return ContainerState(status=status, health=health, exit_code=exit_code)


def _inspect_with_retries(name: str, *, max_errors: int = 5) -> ContainerState:
    errors = 0
    while True:
        try:
            return podman_inspect_state(name)
        except ToolError:
            errors += 1
            if errors >= max_errors:
                raise
            time.sleep(1)


def wait_running(name: str, timeout_sec: int = 120) -> None:
    deadline = time.monotonic() + timeout_sec
    last_reported = ""
    while True:
        state = _inspect_with_retries(name)
        if state.status == "running":
            return
        if state.status in {"exited", "stopped", "dead"}:
            die(
                f"Container '{name}' is not running "
                f"(state='{state.status}', exit_code='{state.exit_code}').",
                exit_code=1,
            )
        if time.monotonic() >= deadline:
            die(
                f"Timed out waiting for '{name}' to be running "
                f"(state='{state.status or 'unknown'}')",
                exit_code=1,
            )
        report = f"state='{state.status or 'unknown'}'"
        if report != last_reported:
            print(f"Waiting for '{name}' to be running ({report})", flush=True)
            last_reported = report
        time.sleep(1)


def wait_healthy(name: str, timeout_sec: int = 120) -> None:
    deadline = time.monotonic() + timeout_sec
    last_reported = ""
    while True:
        state = _inspect_with_retries(name)

        if state.status != "running" and state.status:
            die(
                f"Container '{name}' is not running "
                f"(state='{state.status}', exit_code='{state.exit_code}').",
                exit_code=1,
            )

        # In some environments Podman cannot schedule healthcheck timers.
        # We drive checks manually, matching the repo's compose config.
        run(["podman", "healthcheck", "run", name], check=False, capture=True)

        state = _inspect_with_retries(name)
        if state.health == "healthy":
            return
        if state.health == "unhealthy":
            die(f"Container '{name}' is unhealthy", exit_code=1)
        if time.monotonic() >= deadline:
            health = state.health or "unknown"
            status = state.status or "unknown"
            die(
                f"Timed out waiting for '{name}' to become healthy "
                f"(health='{health}', state='{status}')",
                exit_code=1,
            )

        report = f"state='{state.status or 'unknown'}' health='{state.health or 'unknown'}'"
        if report != last_reported:
            print(f"Waiting for '{name}' to become healthy ({report})", flush=True)
            last_reported = report
        time.sleep(1)
