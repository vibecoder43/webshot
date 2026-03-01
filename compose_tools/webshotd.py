from __future__ import annotations

import os
import re
import signal
import sys
import time
import urllib.error
import urllib.request
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path

from compose_tools.common import ToolError, die, env_with, need_env, tail_lines, wait_for_pid_exit


@dataclass(frozen=True)
class WebshotdPaths:
    state_dir: Path
    pid_file: Path
    log_file: Path


def _state_paths(*, mode: str) -> WebshotdPaths:
    runtime_root = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    state_root = os.environ.get("WEBSHOTD_STATE_DIR") or f"{runtime_root}/webshotd-{os.getuid()}"
    state_dir = Path(state_root) / mode
    state_dir.mkdir(parents=True, exist_ok=True)
    pid_file = state_dir / "webshotd.pid"
    log_file = state_dir / "webshotd.log"
    return WebshotdPaths(state_dir=state_dir, pid_file=pid_file, log_file=log_file)


def _pid_is_running(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return False


def _read_pid(pid_file: Path) -> int | None:
    try:
        raw = pid_file.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def _cpu_limit_is_zero(limit: str) -> bool:
    return re.fullmatch(r"0+([.][0]+)?c", limit) is not None


def _ensure_cpu_limits(env: dict[str, str]) -> dict[str, str]:
    cpu_limit = env.get("CPU_LIMIT", "")
    if cpu_limit:
        if re.fullmatch(r"[0-9]+([.][0-9]+)?c", cpu_limit) is None:
            die(f"CPU_LIMIT must look like '4c' or '0.5c', got: '{cpu_limit}'", exit_code=2)
        if _cpu_limit_is_zero(cpu_limit):
            die(f"CPU_LIMIT must be > 0, got: '{cpu_limit}'", exit_code=2)
        return env

    deploy_vcpu = env.get("DEPLOY_VCPU_LIMIT", "")
    if deploy_vcpu:
        if re.fullmatch(r"[0-9]+([.][0-9]+)?", deploy_vcpu) is None:
            die(
                f"DEPLOY_VCPU_LIMIT must be numeric millicores (e.g. '4000'), got: '{deploy_vcpu}'",
                exit_code=2,
            )
        if re.fullmatch(r"0+([.][0]+)?", deploy_vcpu) is not None:
            die(f"DEPLOY_VCPU_LIMIT must be > 0, got: '{deploy_vcpu}'", exit_code=2)
        return env

    threads = infer_cpu_threads()
    if threads is None:
        die(
            "Failed to infer CPU thread count. Set CPU_LIMIT (e.g. '4c') or "
            "DEPLOY_VCPU_LIMIT (e.g. '4000').",
            exit_code=2,
        )
    return env_with(env, {"CPU_LIMIT": f"{threads}c"})


def _cpuset_cpu_count(cpuset_raw: str) -> int | None:
    cpuset = re.sub(r"\s+", "", cpuset_raw)
    if not cpuset:
        return None
    count = 0
    for part in cpuset.split(","):
        if re.fullmatch(r"[0-9]+", part):
            count += 1
            continue
        m = re.fullmatch(r"([0-9]+)-([0-9]+)", part)
        if m:
            start = int(m.group(1))
            end = int(m.group(2))
            if end < start:
                return None
            count += end - start + 1
            continue
        return None
    return count if count > 0 else None


def _read_trimmed(path: Path) -> str | None:
    try:
        data = path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return None
    data = data.strip()
    return data or None


def infer_cpu_threads() -> int | None:
    threads: int | None = None
    hw_threads = os.cpu_count() or 0

    # cgroup v2
    cgroup_v2_path = None
    cgroup_raw = _read_trimmed(Path("/proc/self/cgroup"))
    if cgroup_raw:
        for line in cgroup_raw.splitlines():
            parts = line.split(":", 2)
            if len(parts) == 3 and parts[0] == "0" and parts[1] == "":
                cgroup_v2_path = parts[2]
                break

    if cgroup_v2_path:
        cg = Path("/sys/fs/cgroup") / cgroup_v2_path.lstrip("/")
        if cg.is_dir():
            cpu_max = _read_trimmed(cg / "cpu.max")
            if cpu_max:
                fields = cpu_max.split()
                if len(fields) == 2 and fields[0].isdigit() and fields[1].isdigit():
                    quota = int(fields[0])
                    period = int(fields[1])
                    if quota > 0 and period > 0:
                        threads = (quota + period - 1) // period

            cpuset_effective = _read_trimmed(cg / "cpuset.cpus.effective")
            if cpuset_effective:
                cpuset_threads = _cpuset_cpu_count(cpuset_effective)
                if cpuset_threads and cpuset_threads > 0:
                    threads = min(threads, cpuset_threads) if threads else cpuset_threads

    # cgroup v1 (best-effort)
    if threads is None:
        cpu_cg_path = None
        if cgroup_raw:
            for line in cgroup_raw.splitlines():
                parts = line.split(":", 2)
                if len(parts) != 3:
                    continue
                controllers = parts[1].split(",") if parts[1] else []
                if "cpu" in controllers:
                    cpu_cg_path = parts[2]
                    break
        if cpu_cg_path:
            candidates = [
                Path("/sys/fs/cgroup/cpu") / cpu_cg_path.lstrip("/"),
                Path("/sys/fs/cgroup/cpu,cpuacct") / cpu_cg_path.lstrip("/"),
            ]
            cpu_mount = next((p for p in candidates if p.is_dir()), None)
            if cpu_mount is not None:
                quota_us = _read_trimmed(cpu_mount / "cpu.cfs_quota_us")
                period_us = _read_trimmed(cpu_mount / "cpu.cfs_period_us")
                if (
                    quota_us
                    and period_us
                    and re.fullmatch(r"-?[0-9]+", quota_us)
                    and period_us.isdigit()
                ):
                    quota = int(quota_us)
                    period = int(period_us)
                    if quota > 0 and period > 0:
                        threads = (quota + period - 1) // period

    if threads is None and hw_threads > 0:
        threads = hw_threads

    if threads is not None and hw_threads > 0 and threads > hw_threads:
        threads = hw_threads

    if threads is not None and threads > 0:
        return threads
    return None


def webshotd_cmd(*, mode: str, repo_root: Path) -> list[str]:
    build_dir = need_env("WEBSHOTD_BUILD_DIR")
    exe = str(Path(build_dir) / "webshotd")
    if mode == "dev":
        return [
            exe,
            "--config",
            str(repo_root / "config/static_config.yaml"),
            "--config_vars",
            str(repo_root / "config/config_vars.debug.yaml"),
        ]
    if mode == "prodlike":
        return [
            exe,
            "--config",
            str(repo_root / "config/static_config.yaml"),
            "--config_vars",
            str(repo_root / "config/config_vars.prod.yaml"),
            "--config_vars_override",
            str(repo_root / "config/config_vars.prod.debug.yaml"),
        ]
    die("mode must be 'dev' or 'prodlike'", exit_code=2)
    raise AssertionError("unreachable")


def webshotd_wait_ready(*, log_file: Path) -> None:
    timeout_sec = int(os.environ.get("WEBSHOTD_READY_TIMEOUT_SEC", "60"))
    deadline = time.monotonic() + timeout_sec

    urls = [
        "http://127.0.0.1:8081/service/monitor?format=json",
        "http://127.0.0.1:8081/service/monitor?format=tskv",
        "http://127.0.0.1:8081/service/monitor",
    ]

    def http_ok() -> bool:
        for url in urls:
            try:
                with urllib.request.urlopen(url, timeout=1) as resp:
                    return resp.status < 500
            except urllib.error.HTTPError as e:
                return e.code < 500
            except Exception:
                continue
        return False

    while True:
        if http_ok():
            return
        if time.monotonic() >= deadline:
            msg = (
                f"webshotd did not become ready within {timeout_sec}s "
                "(monitor listener on 127.0.0.1:8081)."
            )
            print(msg, file=sys.stderr)
            tail = tail_lines(log_file, max_lines=200)
            if tail:
                print("--- webshotd log (tail)", file=sys.stderr)
                for line in tail:
                    print(line, file=sys.stderr)
            raise ToolError(message=msg, exit_code=1)
        time.sleep(1)


def webshotd_start(*, mode: str, repo_root: Path) -> None:
    need_env("WEBSHOTD_RUNTIME_LD_LIBRARY_PATH")

    paths = _state_paths(mode=mode)
    pid = _read_pid(paths.pid_file)
    if pid is not None and _pid_is_running(pid):
        return
    if pid is not None:
        with suppress(FileNotFoundError):
            paths.pid_file.unlink()

    cmd = webshotd_cmd(mode=mode, repo_root=repo_root)
    paths.log_file.parent.mkdir(parents=True, exist_ok=True)
    paths.log_file.touch(exist_ok=True)

    env = _ensure_cpu_limits(dict(os.environ))
    env = env_with(env, {"LD_LIBRARY_PATH": need_env("WEBSHOTD_RUNTIME_LD_LIBRARY_PATH")})

    print(f"Starting webshotd ({mode})...", file=sys.stderr)
    with paths.log_file.open("ab", buffering=0) as log:
        import subprocess

        proc = subprocess.Popen(
            cmd,
            cwd=str(repo_root),
            env=env,
            stdout=log,
            stderr=log,
            start_new_session=True,
        )
    paths.pid_file.write_text(f"{proc.pid}\n", encoding="utf-8")
    webshotd_wait_ready(log_file=paths.log_file)


def webshotd_stop(*, mode: str) -> None:
    timeout_sec = int(os.environ.get("WEBSHOTD_STOP_TIMEOUT_SEC", "30"))
    paths = _state_paths(mode=mode)
    pid = _read_pid(paths.pid_file)
    if pid is None:
        return
    if not _pid_is_running(pid):
        with suppress(FileNotFoundError):
            paths.pid_file.unlink()
        return

    print(f"Stopping webshotd (pid={pid})...", file=sys.stderr)
    with suppress(ProcessLookupError):
        os.kill(pid, signal.SIGTERM)

    if not wait_for_pid_exit(pid, timeout_sec=float(timeout_sec)):
        print(f"webshotd did not stop within {timeout_sec}s, killing pid={pid}.", file=sys.stderr)
        with suppress(ProcessLookupError):
            os.kill(pid, signal.SIGKILL)

    with suppress(FileNotFoundError):
        paths.pid_file.unlink()


def webshotd_status(*, mode: str) -> None:
    paths = _state_paths(mode=mode)
    if paths.pid_file.exists():
        pid = _read_pid(paths.pid_file)
        if pid is not None and _pid_is_running(pid):
            print(f"webshotd: running (pid={pid})")
            return
        print(f"webshotd: stale pid file (pid={pid})")
        return
    print("webshotd: not running (no pid file)")


def webshotd_logs(*, mode: str) -> None:
    paths = _state_paths(mode=mode)
    tail = tail_lines(paths.log_file, max_lines=200)
    for line in tail:
        print(line)

    try:
        with paths.log_file.open("r", encoding="utf-8", errors="replace") as f:
            f.seek(0, os.SEEK_END)
            while True:
                where = f.tell()
                line = f.readline()
                if not line:
                    time.sleep(0.2)
                    f.seek(where)
                    continue
                print(line, end="")
    except FileNotFoundError:
        return
