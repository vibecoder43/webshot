from __future__ import annotations

import argparse
import os
import shlex
import shutil
import signal
import subprocess
import sys
import time
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path

from compose_tools.common import (
    ToolError,
    die,
    need_cmd,
    repo_root_from_file,
    run,
    wait_for_pid_exit,
)
from compose_tools.infra import infra_down, infra_status, infra_up
from compose_tools.resolve_cpu_limit import resolve_cpu_limit
from compose_tools.stack_spec import loadStackSpec

_start_timeout_sec = 10.0
_stop_timeout_sec = 15.0
_podman_cmd_timeout_sec = 10.0
_tail_cmd_timeout_sec = 10.0
_webshotd_ready_url = "http://127.0.0.1:8081/service/monitor?format=json"


@dataclass(frozen=True)
class RuntimeContext:
    mode: str
    repo_root: Path
    state_dir: Path
    config_vars_file: Path
    scan_dir: Path
    svscan_pid_file: Path
    crawlerd_service_dir: Path
    webshotd_service_dir: Path
    crawlerd_socket_path: Path
    crawlerd_log_file: Path
    webshotd_log_file: Path
    binary_path: Path
    config_vars_source: Path
    runtime_ld_library_path: str
    deploy_vcpu_limit: str


def _repo_root() -> Path:
    return repo_root_from_file(Path(__file__))


def _build_context(
    *,
    mode: str,
    binary_path: str,
    config_vars_source: str,
    runtime_ld_library_path: str,
) -> RuntimeContext:
    repo_root = _repo_root()
    state_dir = Path(f"/tmp/webshot-{os.getuid()}") / mode
    scan_dir = state_dir / "s6-scan"
    return RuntimeContext(
        mode=mode,
        repo_root=repo_root,
        state_dir=state_dir,
        config_vars_file=state_dir / "config_vars.yaml",
        scan_dir=scan_dir,
        svscan_pid_file=state_dir / "s6-svscan.pid",
        crawlerd_service_dir=scan_dir / "crawlerd",
        webshotd_service_dir=scan_dir / "webshotd",
        crawlerd_socket_path=state_dir / "crawlerd.sock",
        crawlerd_log_file=state_dir / "crawlerd.log",
        webshotd_log_file=state_dir / "webshotd.log",
        binary_path=Path(binary_path),
        config_vars_source=Path(config_vars_source),
        runtime_ld_library_path=runtime_ld_library_path,
        deploy_vcpu_limit=os.environ.get("DEPLOY_VCPU_LIMIT", ""),
    )


def _service_dirs(ctx: RuntimeContext) -> tuple[Path, Path]:
    return ctx.crawlerd_service_dir, ctx.webshotd_service_dir


def _write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _write_executable(path: Path, content: str) -> None:
    _write_text(path, content)
    path.chmod(0o755)


def _shell_quote(value: str | Path) -> str:
    return shlex.quote(str(value))


def _finish_script() -> str:
    return """#!/bin/sh
if [ "$1" -ne 0 ] && [ "$2" -ne 15 ]; then
  exit 125
fi
exit 0
"""


def _render_runtime_state(ctx: RuntimeContext, *, cpu_limit: str) -> None:
    ctx.state_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ctx.config_vars_source, ctx.config_vars_file)
    with ctx.config_vars_file.open("a", encoding="utf-8") as f:
        f.write(f'crawlerd_socket_path: "{ctx.crawlerd_socket_path}"\n')

    for service_dir in _service_dirs(ctx):
        (service_dir / "data").mkdir(parents=True, exist_ok=True)

    _write_text(ctx.crawlerd_service_dir / "notification-fd", "3\n")
    _write_text(ctx.webshotd_service_dir / "notification-fd", "3\n")

    python_exe = _shell_quote(sys.executable)
    repo_webshotd = _shell_quote(ctx.repo_root / "webshotd")
    crawlerd_socket = _shell_quote(ctx.crawlerd_socket_path)
    crawlerd_log = _shell_quote(ctx.crawlerd_log_file)
    webshotd_log = _shell_quote(ctx.webshotd_log_file)
    binary_path = _shell_quote(ctx.binary_path)
    static_config = _shell_quote(ctx.repo_root / "webshotd/config/static_config.yaml")
    config_vars_file = _shell_quote(ctx.config_vars_file)
    cpu_limit_quoted = _shell_quote(cpu_limit)
    deploy_vcpu_limit = _shell_quote(ctx.deploy_vcpu_limit)
    ld_library_path = _shell_quote(ctx.runtime_ld_library_path)
    webshotd_ready_url = _shell_quote(_webshotd_ready_url)

    _write_executable(
        ctx.crawlerd_service_dir / "data/check",
        (f"#!/bin/sh\nexec {python_exe} -m compose_tools.check_crawlerd_ready {crawlerd_socket}\n"),
    )
    _write_executable(
        ctx.webshotd_service_dir / "data/check",
        (
            "#!/bin/sh\n"
            f"exec {python_exe} -m compose_tools.check_webshotd_ready {webshotd_ready_url}\n"
        ),
    )
    _write_executable(
        ctx.crawlerd_service_dir / "run",
        (
            "#!/bin/sh\n"
            f"exec >>{crawlerd_log} 2>&1\n"
            f"cd {repo_webshotd}\n"
            "exec s6-notifyoncheck -n 0 -w 1000 "
            f"env CPU_LIMIT={cpu_limit_quoted} "
            f"DEPLOY_VCPU_LIMIT={deploy_vcpu_limit} "
            f"crawlerd --socket-path {crawlerd_socket}\n"
        ),
    )
    _write_executable(
        ctx.webshotd_service_dir / "run",
        (
            "#!/bin/sh\n"
            f"exec >>{webshotd_log} 2>&1\n"
            f"cd {repo_webshotd}\n"
            "exec s6-notifyoncheck -n 0 -w 1000 "
            f"env LD_LIBRARY_PATH={ld_library_path} "
            f"CPU_LIMIT={cpu_limit_quoted} "
            f"DEPLOY_VCPU_LIMIT={deploy_vcpu_limit} "
            f"{binary_path} --config {static_config} --config_vars {config_vars_file}\n"
        ),
    )
    _write_executable(ctx.crawlerd_service_dir / "finish", _finish_script())
    _write_executable(ctx.webshotd_service_dir / "finish", _finish_script())


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
    except ProcessLookupError:
        return False
    except PermissionError:
        return False


def _supervisor_running(ctx: RuntimeContext) -> bool:
    need_cmd("s6-svok")
    for service_dir in _service_dirs(ctx):
        proc = run(
            ["s6-svok", str(service_dir)],
            check=False,
            capture=True,
            timeout_sec=_tail_cmd_timeout_sec,
        )
        if proc.returncode != 0:
            return False
    return True


def _start_supervisor(ctx: RuntimeContext) -> None:
    need_cmd("s6-svscan")
    need_cmd("s6-svok")

    pid = _read_pid(ctx.svscan_pid_file)
    if pid is not None and _pid_is_running(pid):
        print("s6: already running")
        return
    if pid is not None:
        with suppress(FileNotFoundError):
            ctx.svscan_pid_file.unlink()

    if not ctx.scan_dir.is_dir():
        die(f"s6: missing scan dir: {ctx.scan_dir}", exit_code=1)

    proc = subprocess.Popen(
        ["s6-svscan", str(ctx.scan_dir)],
        cwd=str(ctx.repo_root),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    ctx.svscan_pid_file.write_text(f"{proc.pid}\n", encoding="utf-8")

    deadline = time.monotonic() + _start_timeout_sec
    while not _supervisor_running(ctx):
        if not _pid_is_running(proc.pid):
            with suppress(FileNotFoundError):
                ctx.svscan_pid_file.unlink()
            die("s6: failed to start supervisor", exit_code=1)
        if time.monotonic() >= deadline:
            with suppress(FileNotFoundError):
                ctx.svscan_pid_file.unlink()
            die("s6: timed out waiting for supervisors", exit_code=1)
        time.sleep(0.1)


def _stop_supervisor(ctx: RuntimeContext) -> None:
    need_cmd("s6-svscanctl")

    pid = _read_pid(ctx.svscan_pid_file)
    if pid is None:
        return
    if not _pid_is_running(pid):
        with suppress(FileNotFoundError):
            ctx.svscan_pid_file.unlink()
        return

    if ctx.scan_dir.is_dir():
        proc = run(
            ["s6-svscanctl", "-t", str(ctx.scan_dir)],
            check=False,
            capture=True,
            timeout_sec=_tail_cmd_timeout_sec,
        )
        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout or "").strip()
            if detail:
                print(detail, file=sys.stderr)
            with suppress(ProcessLookupError):
                os.kill(pid, signal.SIGTERM)
    else:
        with suppress(ProcessLookupError):
            os.kill(pid, signal.SIGTERM)

    if not wait_for_pid_exit(pid, timeout_sec=_stop_timeout_sec):
        with suppress(ProcessLookupError):
            os.kill(pid, signal.SIGTERM)
        if not wait_for_pid_exit(pid, timeout_sec=2.0):
            with suppress(ProcessLookupError):
                os.kill(pid, signal.SIGKILL)
    with suppress(FileNotFoundError):
        ctx.svscan_pid_file.unlink()


def _show_service_status(name: str, service_dir: Path) -> None:
    need_cmd("s6-svstat")
    if (
        run(
            ["s6-svok", str(service_dir)],
            check=False,
            capture=True,
            timeout_sec=_tail_cmd_timeout_sec,
        ).returncode
        != 0
    ):
        print(f"{name}: not supervised")
        return

    proc = run(
        ["s6-svstat", str(service_dir)],
        capture=True,
        timeout_sec=_tail_cmd_timeout_sec,
    )
    print(f"{name}: {proc.stdout.strip()}")


def _spawn_logs(ctx: RuntimeContext) -> list[subprocess.Popen[str]]:
    need_cmd("podman")
    need_cmd("tail")

    spec = loadStackSpec(mode=ctx.mode, compose_dir=ctx.repo_root / "container/compose")
    procs: list[subprocess.Popen[str]] = []
    for container in spec.container_names_all():
        procs.append(
            subprocess.Popen(
                ["podman", "logs", "-f", container],
                cwd=str(ctx.repo_root),
                text=True,
            )
        )

    if _supervisor_running(ctx):
        procs.append(
            subprocess.Popen(
                [
                    "tail",
                    "-n",
                    "+1",
                    "-F",
                    str(ctx.crawlerd_log_file),
                    str(ctx.webshotd_log_file),
                ],
                cwd=str(ctx.repo_root),
                text=True,
            )
        )
    else:
        print("s6: not running")
    return procs


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


def _logs(ctx: RuntimeContext) -> None:
    procs = _spawn_logs(ctx)
    try:
        while True:
            if procs and all(proc.poll() is not None for proc in procs):
                return
            time.sleep(0.2)
    except KeyboardInterrupt:
        raise
    finally:
        _cleanup_processes(procs)


def _up(ctx: RuntimeContext) -> None:
    infra_up(
        mode=ctx.mode, compose_dir=ctx.repo_root / "container/compose", repo_root=ctx.repo_root
    )
    cpu_limit = resolve_cpu_limit()
    _render_runtime_state(ctx, cpu_limit=cpu_limit)
    _start_supervisor(ctx)


def _down(ctx: RuntimeContext) -> None:
    _stop_supervisor(ctx)
    infra_down(mode=ctx.mode, compose_dir=ctx.repo_root / "container/compose")
    if ctx.state_dir.exists():
        shutil.rmtree(ctx.state_dir)


def _status(ctx: RuntimeContext) -> None:
    infra_status(mode=ctx.mode, compose_dir=ctx.repo_root / "container/compose")
    if _supervisor_running(ctx):
        _show_service_status("crawlerd", ctx.crawlerd_service_dir)
        _show_service_status("webshotd", ctx.webshotd_service_dir)
    else:
        print("s6: not running")


def main() -> int:
    parser = argparse.ArgumentParser(prog="compose_tools.webshot_runtime")
    parser.add_argument("action", choices=["up", "down", "status", "logs"])
    parser.add_argument("--mode", required=True, choices=["dev", "prodlike"])
    parser.add_argument("--binary-path", required=True)
    parser.add_argument("--config-vars-source", required=True)
    parser.add_argument("--runtime-ld-library-path", required=True)
    args = parser.parse_args()

    ctx = _build_context(
        mode=args.mode,
        binary_path=args.binary_path,
        config_vars_source=args.config_vars_source,
        runtime_ld_library_path=args.runtime_ld_library_path,
    )

    try:
        if args.action == "up":
            _up(ctx)
        elif args.action == "down":
            _down(ctx)
        elif args.action == "status":
            _status(ctx)
        elif args.action == "logs":
            _logs(ctx)
        else:
            raise AssertionError("unreachable")
        return 0
    except ToolError as e:
        if e.message:
            print(e.message, file=sys.stderr)
        return e.exit_code
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
