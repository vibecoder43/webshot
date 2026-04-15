from __future__ import annotations

import os
import signal
import subprocess
import time
from contextlib import suppress
from pathlib import Path

from s6.common import need_cmd, run, tail_lines, wait_for_pid_exit
from s6.runtime_context import (
    RuntimeInspectContext,
    RuntimeStateContext,
    RuntimeUpContext,
    ServiceSpec,
)
from s6.runtime_services import active_service_specs, known_service_dirs
from s6.runtime_support import report, runtime_die

START_TIMEOUT_SEC = 120.0
STOP_TIMEOUT_SEC = 15.0
CMD_TIMEOUT_SEC = 30.0
POLL_INTERVAL_SEC = 0.2
LOGS_WAIT_TIMEOUT_SEC = 5.0


def supervisor_running(ctx: RuntimeInspectContext) -> bool:
    need_cmd("s6-svok")
    return all(
        run(["s6-svok", str(spec.service_dir)], check=False, timeout_sec=CMD_TIMEOUT_SEC).returncode
        == 0
        for spec in active_service_specs(ctx)
    )


def supervisor_matches_profile(ctx: RuntimeInspectContext) -> bool:
    need_cmd("s6-svok")
    active_names = {spec.name for spec in active_service_specs(ctx)}
    for name, service_dir in known_service_dirs(ctx).items():
        supervised = (
            run(["s6-svok", str(service_dir)], check=False, timeout_sec=CMD_TIMEOUT_SEC).returncode
            == 0
        )
        if name in active_names:
            if not supervised:
                return False
        elif supervised:
            return False
    return True


def stack_healthy(ctx: RuntimeInspectContext) -> bool:
    if not supervisor_running(ctx):
        return False
    return all(
        run(spec.ready_cmd, check=False, timeout_sec=CMD_TIMEOUT_SEC).returncode == 0
        for spec in active_service_specs(ctx)
    )


def start_supervisor(ctx: RuntimeUpContext, services: list[ServiceSpec]) -> None:
    need_cmd("s6-svscan")

    pid = _read_pid(ctx.svscan_pid_file)
    if pid is not None and _pid_is_running(pid):
        report("already running")
        return
    if pid is not None:
        with suppress(FileNotFoundError):
            ctx.svscan_pid_file.unlink()

    proc = subprocess.Popen(
        ["s6-svscan", str(ctx.scan_dir)],
        cwd=str(ctx.repo_root),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    ctx.svscan_pid_file.write_text(f"{proc.pid}\n", encoding="utf-8")

    deadline = time.monotonic() + START_TIMEOUT_SEC
    while not supervisor_running(ctx):
        if not _pid_is_running(proc.pid):
            runtime_die("failed to start supervisor", exit_code=1)
        if time.monotonic() >= deadline:
            runtime_die("timed out waiting for supervisors", exit_code=1)
        time.sleep(0.1)

    for service in services:
        _wait_ready(service)


def stop_supervisor(ctx: RuntimeStateContext) -> None:
    need_cmd("s6-svscanctl")

    pid = _read_pid(ctx.svscan_pid_file)
    if pid is None:
        return
    if not _pid_is_running(pid):
        with suppress(FileNotFoundError):
            ctx.svscan_pid_file.unlink()
        return

    proc = run(
        ["s6-svscanctl", "-t", str(ctx.scan_dir)],
        check=False,
        capture=True,
        timeout_sec=CMD_TIMEOUT_SEC,
    )
    if proc.returncode != 0:
        with suppress(ProcessLookupError):
            os.kill(pid, signal.SIGTERM)

    if not wait_for_pid_exit(pid, timeout_sec=STOP_TIMEOUT_SEC):
        with suppress(ProcessLookupError):
            os.kill(pid, signal.SIGTERM)
        if not wait_for_pid_exit(pid, timeout_sec=2.0):
            with suppress(ProcessLookupError):
                os.kill(pid, signal.SIGKILL)
    with suppress(FileNotFoundError):
        ctx.svscan_pid_file.unlink()


def logs(ctx: RuntimeInspectContext) -> None:
    procs = _spawn_logs(ctx)
    try:
        while True:
            if procs and all(proc.poll() is not None for proc in procs):
                return
            time.sleep(0.2)
    finally:
        _cleanup_processes(procs)


def status(ctx: RuntimeInspectContext) -> None:
    if not supervisor_running(ctx):
        report("not running")
        return
    for spec in active_service_specs(ctx):
        _show_service_status(spec)


def check(ctx: RuntimeInspectContext) -> int:
    return 0 if stack_healthy(ctx) else 1


def _wait_ready(service: ServiceSpec) -> None:
    deadline = time.monotonic() + service.timeout_sec
    while time.monotonic() < deadline:
        if run(service.ready_cmd, check=False, timeout_sec=CMD_TIMEOUT_SEC).returncode == 0:
            return
        time.sleep(POLL_INTERVAL_SEC)

    tail = "\n".join(tail_lines(service.log_file, max_lines=40))
    detail = f"\n{tail}" if tail else ""
    runtime_die(f"Timed out waiting for {service.name} readiness{detail}", exit_code=1)


def _read_pid(path: Path) -> int | None:
    try:
        raw = path.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def _pid_is_running(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except (PermissionError, ProcessLookupError):
        return False


def _show_service_status(spec: ServiceSpec) -> None:
    need_cmd("s6-svstat")
    if (
        run(["s6-svok", str(spec.service_dir)], check=False, timeout_sec=CMD_TIMEOUT_SEC).returncode
        != 0
    ):
        print(f"{spec.name}: not supervised")
        return
    proc = run(["s6-svstat", str(spec.service_dir)], capture=True, timeout_sec=CMD_TIMEOUT_SEC)
    print(f"{spec.name}: {proc.stdout.strip()}")


def _wait_for_log_files(ctx: RuntimeInspectContext) -> list[Path]:
    deadline = time.monotonic() + LOGS_WAIT_TIMEOUT_SEC
    missing = [spec.log_file for spec in active_service_specs(ctx) if not spec.log_file.is_file()]
    while missing and time.monotonic() < deadline:
        time.sleep(POLL_INTERVAL_SEC)
        missing = [path for path in missing if not path.is_file()]
    return missing


def _require_logs_ready(ctx: RuntimeInspectContext) -> list[Path]:
    if not supervisor_running(ctx):
        runtime_die(f"not running; run proj:{ctx.mode}Up first before reading logs", exit_code=1)

    missing = _wait_for_log_files(ctx)
    if missing:
        missing_paths = "\n".join(str(path) for path in missing)
        runtime_die(f"missing expected log files while supervised:\n{missing_paths}", exit_code=1)
    return [spec.log_file for spec in active_service_specs(ctx)]


def _spawn_logs(ctx: RuntimeInspectContext) -> list[subprocess.Popen[str]]:
    need_cmd("tail")
    log_files = [str(path) for path in _require_logs_ready(ctx)]
    report(f"attaching {ctx.mode} logs")
    return [
        subprocess.Popen(
            ["tail", "-n", "+1", "-F", *log_files],
            cwd=str(ctx.repo_root),
            text=True,
        )
    ]


def _cleanup_processes(procs: list[subprocess.Popen[str]]) -> None:
    for proc in procs:
        if proc.poll() is None:
            with suppress(Exception):
                proc.terminate()
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if all(proc.poll() is not None for proc in procs):
            return
        time.sleep(0.1)
    for proc in procs:
        if proc.poll() is None:
            with suppress(Exception):
                proc.kill()
