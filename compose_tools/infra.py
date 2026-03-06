from __future__ import annotations

import os
import subprocess
import sys
import threading
import time
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path

from compose_tools.common import ToolError, die, need_cmd, run, wait_for_pid_exit
from compose_tools.podman_compose import (
    ContainerState,
    compose,
    podman_inspect_state,
    require_podman_compose,
    wait_healthy,
    wait_running,
)
from compose_tools.s3_bucket import ensure_s3_bucket_exists
from compose_tools.stack_spec import StackSpec, loadStackSpec

_squid_load_timeout_sec = 1800.0
_compose_up_timeout_sec = 1800.0
_podman_cmd_timeout_sec = 10.0
_podman_slow_timeout_sec = 30.0


@dataclass(frozen=True)
class InfraSupervisorPaths:
    state_dir: Path
    pid_file: Path
    log_file: Path


def _supervisor_state_paths(*, mode: str) -> InfraSupervisorPaths:
    runtime_root = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
    state_root = os.environ.get("INFRA_SUPERVISOR_STATE_DIR") or (
        f"{runtime_root}/infra-supervisor-{os.getuid()}"
    )
    state_dir = Path(state_root) / mode
    state_dir.mkdir(parents=True, exist_ok=True)
    pid_file = state_dir / "infra_supervisor.pid"
    log_file = state_dir / "infra_supervisor.log"
    return InfraSupervisorPaths(state_dir=state_dir, pid_file=pid_file, log_file=log_file)


def _pid_is_running(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return False


def _read_pid(path: Path) -> int | None:
    try:
        raw = path.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def infra_supervisor_start(*, mode: str, compose_dir: Path, repo_root: Path) -> None:
    paths = _supervisor_state_paths(mode=mode)
    pid = _read_pid(paths.pid_file)
    if pid is not None and _pid_is_running(pid):
        return
    if pid is not None:
        with suppress(FileNotFoundError):
            paths.pid_file.unlink()

    cmd = [
        sys.executable,
        str(repo_root / "container/compose/infra.py"),
        mode,
        "watch",
    ]

    paths.log_file.parent.mkdir(parents=True, exist_ok=True)
    paths.log_file.touch(exist_ok=True)

    with paths.log_file.open("ab", buffering=0) as log:
        proc = subprocess.Popen(
            cmd,
            cwd=str(repo_root),
            env=dict(os.environ),
            stdout=log,
            stderr=log,
            start_new_session=True,
        )
    paths.pid_file.write_text(f"{proc.pid}\n", encoding="utf-8")


def infra_supervisor_stop(*, mode: str) -> None:
    import signal

    timeout_sec = int(os.environ.get("INFRA_SUPERVISOR_STOP_TIMEOUT_SEC", "5"))
    paths = _supervisor_state_paths(mode=mode)
    pid = _read_pid(paths.pid_file)
    if pid is None:
        return
    if not _pid_is_running(pid):
        with suppress(FileNotFoundError):
            paths.pid_file.unlink()
        return

    with suppress(ProcessLookupError):
        os.kill(pid, signal.SIGTERM)
    if not wait_for_pid_exit(pid, timeout_sec=float(timeout_sec)):
        with suppress(ProcessLookupError):
            os.kill(pid, signal.SIGKILL)
    with suppress(FileNotFoundError):
        paths.pid_file.unlink()


def ensure_networks() -> None:
    need_cmd("podman")

    def ensure_network(name: str, want_internal: str) -> None:
        inspect = run(
            ["podman", "network", "inspect", name],
            check=False,
            capture=True,
            timeout_sec=_podman_cmd_timeout_sec,
        )
        if inspect.returncode == 0:
            internal = run(
                ["podman", "network", "inspect", "-f", "{{.Internal}}", name],
                capture=True,
                check=False,
                timeout_sec=_podman_cmd_timeout_sec,
            ).stdout.strip()
            if internal != want_internal:
                print(
                    f"Podman network '{name}' has Internal='{internal}', "
                    f"expected '{want_internal}'.",
                    file=sys.stderr,
                )
                print("Bring down containers that use it and recreate it:", file=sys.stderr)
                print(f"  podman network rm '{name}'", file=sys.stderr)
                if want_internal == "true":
                    print(f"  podman network create --internal '{name}'", file=sys.stderr)
                else:
                    print(f"  podman network create '{name}'", file=sys.stderr)
                raise ToolError(message="", exit_code=1)
            return

        if want_internal == "true":
            run(
                ["podman", "network", "create", "--internal", name],
                timeout_sec=_podman_slow_timeout_sec,
            )
        else:
            run(["podman", "network", "create", name], timeout_sec=_podman_slow_timeout_sec)

    ensure_network("crawler_net", "true")
    ensure_network("egress_net", "false")


def infra_ready(*, mode: str, compose_dir: Path, verbose: bool = False) -> bool:
    need_cmd("podman")
    spec = loadStackSpec(mode=mode, compose_dir=compose_dir)
    names = spec.container_names_all()

    for name in names:
        state: ContainerState | None = None
        try:
            state = podman_inspect_state(name)
        except ToolError:
            if verbose:
                print(f"infra: {name}: missing", file=sys.stderr)
                run(
                    [
                        "podman",
                        "ps",
                        "-a",
                        "--filter",
                        f"name=^{name}$",
                        "--format",
                        "infra: {{.Names}}: {{.Status}}",
                    ],
                    check=False,
                    timeout_sec=_podman_cmd_timeout_sec,
                )
            return False

        if state.status != "running":
            if verbose:
                print(f"infra: {name}: not running (status='{state.status}')", file=sys.stderr)
                run(
                    [
                        "podman",
                        "ps",
                        "-a",
                        "--filter",
                        f"name=^{name}$",
                        "--format",
                        "infra: {{.Names}}: {{.Status}}",
                    ],
                    check=False,
                    timeout_sec=_podman_cmd_timeout_sec,
                )
                run(
                    ["podman", "logs", "--tail=50", name],
                    check=False,
                    timeout_sec=_podman_cmd_timeout_sec,
                )
            return False

        if state.health and state.health != "healthy":
            if verbose:
                print(f"infra: {name}: health='{state.health}'", file=sys.stderr)
                run(
                    ["podman", "logs", "--tail=50", name],
                    check=False,
                    timeout_sec=_podman_cmd_timeout_sec,
                )
            return False

    return True


def infra_status(*, mode: str, compose_dir: Path) -> None:
    print(f"Mode: {mode}")
    if infra_ready(mode=mode, compose_dir=compose_dir):
        print("Infra: ready")
    else:
        print("Infra: not ready")
        infra_ready(mode=mode, compose_dir=compose_dir, verbose=True)


def assert_servicedb_timezone_utc(*, container_name: str = "servicedb") -> None:
    tz_raw = run(
        ["podman", "exec", container_name, "psql", "-U", "postgres", "-tAqc", "SHOW TimeZone;"],
        capture=True,
        check=False,
        timeout_sec=_podman_cmd_timeout_sec,
    )
    if tz_raw.returncode != 0:
        print(
            f"Failed to read Postgres TimeZone from container '{container_name}':", file=sys.stderr
        )
        print(tz_raw.stdout, file=sys.stderr)
        print(tz_raw.stderr, file=sys.stderr)
        raise ToolError(message="Failed to read Postgres TimeZone", exit_code=1)

    log_tz_raw = run(
        ["podman", "exec", container_name, "psql", "-U", "postgres", "-tAqc", "SHOW log_timezone;"],
        capture=True,
        check=False,
        timeout_sec=_podman_cmd_timeout_sec,
    )
    if log_tz_raw.returncode != 0:
        print(
            f"Failed to read Postgres log_timezone from container '{container_name}':",
            file=sys.stderr,
        )
        print(log_tz_raw.stdout, file=sys.stderr)
        print(log_tz_raw.stderr, file=sys.stderr)
        raise ToolError(message="Failed to read Postgres log_timezone", exit_code=1)

    tz = "".join(tz_raw.stdout.split())
    log_tz = "".join(log_tz_raw.stdout.split())
    if not tz or not log_tz:
        raise ToolError(
            message=f"Failed to read Postgres timezone settings from container '{container_name}'.",
            exit_code=1,
        )
    if tz != "UTC":
        raise ToolError(
            message=f"Postgres TimeZone must be UTC, got '{tz}' (container='{container_name}').",
            exit_code=1,
        )
    if log_tz != "UTC":
        raise ToolError(
            message=(
                f"Postgres log_timezone must be UTC, got '{log_tz}' (container='{container_name}')."
            ),
            exit_code=1,
        )


def _compose_pod_name(*, compose_dir: Path) -> str:
    return f"pod_{compose_dir.name}"


def infra_down_compose(*, compose_dir: Path, compose_file: str) -> None:
    require_podman_compose()
    timeout_sec = int(os.environ.get("INFRA_DOWN_TIMEOUT_SEC", "90"))
    print(
        f"infra: compose down ({compose_file}) (timeout={timeout_sec}s)...",
        file=sys.stderr,
        flush=True,
    )
    try:
        proc = run(
            ["podman", "compose", "--in-pod", "true", "-f", compose_file, "down"],
            cwd=compose_dir,
            timeout_sec=float(timeout_sec),
            capture=True,
            check=False,
        )
    except ToolError:
        proc = None
    else:
        if proc.returncode == 0:
            return

    pod_name = _compose_pod_name(compose_dir=compose_dir)
    pod_inspect = run(
        ["podman", "pod", "inspect", pod_name],
        check=False,
        capture=True,
        timeout_sec=_podman_cmd_timeout_sec,
    )
    if pod_inspect.returncode != 0:
        print(f"Infra is already down (pod '{pod_name}' not found).", file=sys.stderr)
        return

    print(
        f"Warning: podman compose down failed/timed out after {timeout_sec}s; "
        f"forcing pod removal: {pod_name}",
        file=sys.stderr,
    )
    run(["podman", "pod", "rm", "-f", pod_name], check=False, timeout_sec=_podman_slow_timeout_sec)
    run(
        ["podman", "pod", "inspect", pod_name],
        check=False,
        timeout_sec=_podman_cmd_timeout_sec,
    )


def ensure_servicedb_timezone_utc_or_down(
    *, compose_dir: Path, compose_file: str, container_name: str = "servicedb"
) -> None:
    try:
        assert_servicedb_timezone_utc(container_name=container_name)
    except ToolError:
        with suppress(ToolError):
            infra_down_compose(compose_dir=compose_dir, compose_file=compose_file)
        raise


def _squid_load(mode: str) -> None:
    if mode == "dev":
        cmd = "squid_load_dev"
    elif mode == "prodlike":
        cmd = "squid_load_prodlike"
    else:
        die("mode must be 'dev' or 'prodlike'", exit_code=2)
    need_cmd(cmd)
    run([cmd], timeout_sec=_squid_load_timeout_sec)


def _squid_image_tag(mode: str) -> str:
    if mode == "dev":
        return "localhost/squid:dev"
    if mode == "prodlike":
        return "localhost/squid:prodlike"
    die("mode must be 'dev' or 'prodlike'", exit_code=2)
    raise AssertionError("unreachable")


def ensure_squid_image_loaded(*, mode: str) -> None:
    need_cmd("podman")
    require_podman_compose()
    tag = _squid_image_tag(mode)
    inspect = run(
        ["podman", "image", "inspect", tag],
        check=False,
        capture=True,
        timeout_sec=_podman_cmd_timeout_sec,
    )
    if inspect.returncode == 0:
        return
    print(f"infra: squid image missing, loading: {tag}", file=sys.stderr, flush=True)
    _squid_load(mode)


def _ensure_s3_bucket_dev(*, repo_root: Path) -> None:
    secdist_path = repo_root / "secret/test_secdist.json"
    endpoint = "localhost:8333"
    bucket = "webshot"
    timeout_sec = 30
    retry_delay_sec = 1

    deadline = time.monotonic() + timeout_sec
    while True:
        try:
            ensure_s3_bucket_exists(secrets_path=secdist_path, endpoint=endpoint, bucket=bucket)
            return
        except ToolError as e:
            if e.exit_code == 2:
                raise
        except Exception:
            pass
        if time.monotonic() >= deadline:
            raise ToolError(
                message=(
                    f"Timed out ensuring S3 bucket exists: bucket='{bucket}' endpoint='{endpoint}'"
                ),
                exit_code=1,
            )
        time.sleep(retry_delay_sec)


def _stack_wait_ready(*, spec: StackSpec) -> None:
    for name in spec.container_names_healthchecked():
        wait_healthy(name, 120)
    for name in spec.container_names_non_healthchecked():
        wait_running(name, 120)


def _dump_diagnostics(name: str) -> None:
    run(
        [
            "podman",
            "ps",
            "-a",
            "--filter",
            f"name=^{name}$",
            "--format",
            "infra: {{.Names}}: {{.Status}}",
        ],
        check=False,
        timeout_sec=_podman_cmd_timeout_sec,
    )
    run(["podman", "logs", "--tail=200", name], check=False, timeout_sec=_podman_cmd_timeout_sec)


def _read_float_env(name: str, default: str) -> float:
    raw = os.environ.get(name, default)
    try:
        value = float(raw)
    except ValueError:
        die(f"{name} must be a number, got: {raw!r}", exit_code=2)
    return value


def _read_int_env(name: str, default: str) -> int:
    raw = os.environ.get(name, default)
    try:
        value = int(raw)
    except ValueError:
        die(f"{name} must be an int, got: {raw!r}", exit_code=2)
    return value


def _stack_supervise(*, spec: StackSpec, compose_dir: Path) -> None:
    need_cmd("podman")
    require_podman_compose()

    period_sec = _read_float_env("INFRA_HEALTHCHECK_PERIOD_SEC", "10")
    if period_sec <= 0:
        die(f"INFRA_HEALTHCHECK_PERIOD_SEC must be > 0, got: {period_sec!r}", exit_code=2)

    max_fails = _read_int_env("INFRA_HEALTHCHECK_FAILS", "3")
    if max_fails <= 0:
        die(f"INFRA_HEALTHCHECK_FAILS must be > 0, got: {max_fails!r}", exit_code=2)

    names_all = spec.container_names_all()
    names_health = spec.container_names_healthchecked()
    compose_file = spec.compose_file.name

    stop_event = threading.Event()
    failure_event = threading.Event()
    failure_lock = threading.Lock()
    failure_name = ""
    failure_reason = ""

    def set_failure(name: str, reason: str) -> None:
        nonlocal failure_name, failure_reason
        with failure_lock:
            if failure_event.is_set():
                return
            failure_name = name
            failure_reason = reason
            failure_event.set()
            stop_event.set()

    procs: dict[str, subprocess.Popen[str]] = {}

    def stop_waiter(name: str, proc: subprocess.Popen[str]) -> None:
        out, err = proc.communicate()
        if stop_event.is_set():
            return
        if proc.returncode != 0:
            detail = (err or out or "").strip()
            suffix = f": {detail}" if detail else ""
            set_failure(name, f"wait failed (exit={proc.returncode}){suffix}")
            return

        exit_code_raw = (out or "").strip().splitlines()
        exit_code = exit_code_raw[0].strip() if exit_code_raw else ""
        reason = f"stopped (exit_code={exit_code})" if exit_code else "stopped"
        set_failure(name, reason)

    for name in names_all:
        proc = subprocess.Popen(
            ["podman", "wait", "--condition", "stopped", "--interval", "2s", name],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        procs[name] = proc
        thread = threading.Thread(target=stop_waiter, args=(name, proc), daemon=True)
        thread.start()

    def health_driver() -> None:
        try:
            fails: dict[str, int] = {name: 0 for name in names_health}
            while not stop_event.is_set():
                for name in names_health:
                    if stop_event.is_set():
                        return
                    proc = run(
                        ["podman", "healthcheck", "run", name],
                        check=False,
                        capture=True,
                        timeout_sec=_podman_cmd_timeout_sec,
                    )
                    if proc.returncode == 0:
                        fails[name] = 0
                        continue
                    fails[name] += 1
                    if fails[name] >= max_fails:
                        set_failure(name, f"healthcheck failed {fails[name]} times")
                        return
                stop_event.wait(period_sec)
        except Exception as e:
            set_failure("<health_driver>", f"crashed: {e}")

    health_thread = threading.Thread(target=health_driver, daemon=True)
    health_thread.start()

    try:
        while not failure_event.is_set():
            failure_event.wait(timeout=0.2)
    except KeyboardInterrupt:
        stop_event.set()
        for proc in procs.values():
            if proc.poll() is None:
                with suppress(Exception):
                    proc.terminate()
        time.sleep(0.2)
        for proc in procs.values():
            if proc.poll() is None:
                with suppress(Exception):
                    proc.kill()
        raise

    for proc in procs.values():
        if proc.poll() is None:
            with suppress(Exception):
                proc.terminate()
    time.sleep(0.2)
    for proc in procs.values():
        if proc.poll() is None:
            with suppress(Exception):
                proc.kill()

    with failure_lock:
        name = failure_name or "<unknown>"
        reason = failure_reason or "unknown failure"

    print(f"infra: {name}: {reason}", file=sys.stderr)
    with suppress(ToolError):
        if name != "<unknown>":
            _dump_diagnostics(name)
    with suppress(ToolError):
        infra_down_compose(compose_dir=compose_dir, compose_file=compose_file)
    die(f"Infra became not ready: {name}: {reason}", exit_code=1)


def infra_watch(*, mode: str, compose_dir: Path) -> None:
    spec = loadStackSpec(mode=mode, compose_dir=compose_dir)
    _stack_wait_ready(spec=spec)
    _stack_supervise(spec=spec, compose_dir=compose_dir)


def infra_supervise(*, mode: str, compose_dir: Path, repo_root: Path) -> None:
    infra_up(mode=mode, compose_dir=compose_dir, repo_root=repo_root)
    spec = loadStackSpec(mode=mode, compose_dir=compose_dir)
    try:
        _stack_supervise(spec=spec, compose_dir=compose_dir)
    except KeyboardInterrupt:
        with suppress(ToolError):
            infra_down(mode=mode, compose_dir=compose_dir)
        raise


def infra_up(*, mode: str, compose_dir: Path, repo_root: Path) -> None:
    print(f"infra: up ({mode})", file=sys.stderr, flush=True)
    ensure_networks()
    ensure_squid_image_loaded(mode=mode)

    spec = loadStackSpec(mode=mode, compose_dir=compose_dir)
    compose_file = spec.compose_file.name

    compose_attempted = False
    try:
        print("infra: starting containers...", file=sys.stderr, flush=True)
        compose_attempted = True
        compose(
            ["--in-pod", "true", "-f", compose_file, "up", "-d"],
            cwd=compose_dir,
            timeout_sec=_compose_up_timeout_sec,
        )

        print("infra: waiting for readiness...", file=sys.stderr, flush=True)
        _stack_wait_ready(spec=spec)
        ensure_servicedb_timezone_utc_or_down(
            compose_dir=compose_dir,
            compose_file=compose_file,
            container_name=spec.service_container_name("servicedb"),
        )

        if mode == "dev":
            print("infra: ensuring dev S3 bucket...", file=sys.stderr, flush=True)
            _ensure_s3_bucket_dev(repo_root=repo_root)

    except ToolError:
        if compose_attempted:
            print("infra: failed; bringing infra down...", file=sys.stderr, flush=True)
            with suppress(ToolError):
                infra_down_compose(compose_dir=compose_dir, compose_file=compose_file)
        raise


def infra_down(*, mode: str, compose_dir: Path) -> None:
    print(f"infra: down ({mode})", file=sys.stderr, flush=True)
    spec = loadStackSpec(mode=mode, compose_dir=compose_dir)
    compose_file = spec.compose_file.name
    print("infra: stopping supervisor...", file=sys.stderr, flush=True)
    infra_supervisor_stop(mode=mode)
    print("infra: bringing down containers...", file=sys.stderr, flush=True)
    infra_down_compose(compose_dir=compose_dir, compose_file=compose_file)
