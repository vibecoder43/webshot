from __future__ import annotations

import argparse
import os
import shlex
import shutil
import signal
import ssl
import subprocess
import sys
import time
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

import yaml

from s6.common import (
    ToolError,
    die,
    need_cmd,
    repo_root_from_file,
    run,
    tail_lines,
    wait_for_pid_exit,
)
from s6.resolve_cpu_limit import resolve_cpu_limit
from s6.s3_bucket import ensure_s3_bucket_exists

_start_timeout_sec = 120.0
_stop_timeout_sec = 15.0
_cmd_timeout_sec = 30.0
_poll_interval_sec = 0.2
_webshotd_ready_url = "http://127.0.0.1:8081/service/monitor?format=json"


@dataclass(frozen=True)
class ServiceSpec:
    name: str
    service_dir: Path
    log_file: Path
    ready_cmd: list[str]
    timeout_sec: float


@dataclass(frozen=True)
class RuntimeContext:
    mode: str
    repo_root: Path
    state_dir: Path
    scan_dir: Path
    svscan_pid_file: Path
    binary_path: Path
    config_vars_source: Path
    runtime_ld_library_path: str
    deploy_vcpu_limit: str
    crawlerd_socket_path: Path
    postgres_data_dir: Path
    postgres_run_dir: Path
    postgres_bootstrap_done_file: Path
    postgres_bootstrap_log: Path
    postgres_log_file: Path
    postgres_capture_meta_db_name: str
    postgres_shared_state_db_name: str
    proxy_dir: Path
    proxy_confdir: Path
    proxy_log_file: Path
    proxy_upstream_ca_file: Path
    seaweed_data_dir: Path
    seaweed_log_file: Path
    test_target_dir: Path
    test_target_config_path: Path
    test_target_log_file: Path
    crawlerd_service_dir: Path
    crawlerd_log_file: Path
    webshotd_service_dir: Path
    webshotd_log_file: Path


def _repo_root() -> Path:
    return repo_root_from_file(Path(__file__))


def _shell_quote(value: str | Path) -> str:
    return shlex.quote(str(value))


def _shell_join(parts: list[str | Path]) -> str:
    return " ".join(_shell_quote(part) for part in parts)


def _write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _write_bytes(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def _write_executable(path: Path, content: str) -> None:
    _write_text(path, content)
    path.chmod(0o755)


def _read_yaml(path: Path) -> dict[str, object]:
    try:
        raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    except FileNotFoundError as e:
        raise ToolError(message=f"Missing config vars file: {path}", exit_code=2) from e

    if not isinstance(raw, dict):
        die(f"Config vars file must be a YAML mapping: {path}", exit_code=2)
    return raw


def _require_yaml_path(raw: dict[str, object], key: str, *, source: Path) -> Path:
    value = raw.get(key)
    if not isinstance(value, str) or not value:
        die(f"Missing required config var '{key}' in {source}", exit_code=2)
    return Path(value)


def _require_yaml_string(raw: dict[str, object], key: str, *, source: Path) -> str:
    value = raw.get(key)
    if not isinstance(value, str) or not value:
        die(f"Missing required config var '{key}' in {source}", exit_code=2)
    return value


def _database_name_from_dsn(dsn: str, *, key: str, source: Path) -> str:
    parsed = urlsplit(dsn)
    db_name = parsed.path.lstrip("/")
    if not db_name:
        die(f"Config var '{key}' in {source} must include a database name", exit_code=2)
    return db_name


def _fixed_state_dir(mode: str) -> Path:
    return Path("/tmp/webshot") / mode


def _build_context(
    *,
    mode: str,
    binary_path: str,
    config_vars_source: str,
    runtime_ld_library_path: str,
) -> RuntimeContext:
    repo_root = _repo_root()
    config_path = Path(config_vars_source)
    raw_vars = _read_yaml(config_path)
    state_dir = _fixed_state_dir(mode)
    crawlerd_socket_path = _require_yaml_path(raw_vars, "crawlerd_socket_path", source=config_path)
    capture_meta_db_name = _database_name_from_dsn(
        _require_yaml_string(raw_vars, "pg_capture_meta_db_dsn", source=config_path),
        key="pg_capture_meta_db_dsn",
        source=config_path,
    )
    shared_state_db_name = _database_name_from_dsn(
        _require_yaml_string(raw_vars, "pg_shared_state_db_dsn", source=config_path),
        key="pg_shared_state_db_dsn",
        source=config_path,
    )
    expected_socket_path = state_dir / "crawlerd.sock"
    if crawlerd_socket_path != expected_socket_path:
        die(
            "Expected "
            f"crawlerd_socket_path={expected_socket_path} in {config_path}, "
            f"got {crawlerd_socket_path}",
            exit_code=2,
        )
    scan_dir = state_dir / "s6-scan"
    return RuntimeContext(
        mode=mode,
        repo_root=repo_root,
        state_dir=state_dir,
        scan_dir=scan_dir,
        svscan_pid_file=state_dir / "s6-svscan.pid",
        binary_path=Path(binary_path),
        config_vars_source=config_path,
        runtime_ld_library_path=runtime_ld_library_path,
        deploy_vcpu_limit=os.environ.get("DEPLOY_VCPU_LIMIT", ""),
        crawlerd_socket_path=crawlerd_socket_path,
        postgres_data_dir=state_dir / "postgres" / "data",
        postgres_run_dir=state_dir / "postgres" / "run",
        postgres_bootstrap_done_file=state_dir / "postgres" / ".bootstrap-complete",
        postgres_bootstrap_log=state_dir / "postgres-bootstrap.log",
        postgres_log_file=state_dir / "postgres.log",
        postgres_capture_meta_db_name=capture_meta_db_name,
        postgres_shared_state_db_name=shared_state_db_name,
        proxy_dir=state_dir / "mitmproxy",
        proxy_confdir=state_dir / "mitmproxy" / "confdir",
        proxy_log_file=state_dir / "mitmproxy.log",
        proxy_upstream_ca_file=state_dir / "mitmproxy" / "upstream-ca.pem",
        seaweed_data_dir=state_dir / "seaweed",
        seaweed_log_file=state_dir / "seaweedfs.log",
        test_target_dir=state_dir / "test-target",
        test_target_config_path=state_dir / "test-target" / "nginx.conf",
        test_target_log_file=state_dir / "test-target.log",
        crawlerd_service_dir=scan_dir / "crawlerd",
        crawlerd_log_file=state_dir / "crawlerd.log",
        webshotd_service_dir=scan_dir / "webshotd",
        webshotd_log_file=state_dir / "webshotd.log",
    )


def _service_specs(ctx: RuntimeContext) -> list[ServiceSpec]:
    python_exe = sys.executable
    specs = [
        ServiceSpec(
            name="postgres",
            service_dir=ctx.scan_dir / "postgres",
            log_file=ctx.postgres_log_file,
            ready_cmd=["pg_isready", "-h", "127.0.0.1", "-p", "5432", "-U", "postgres"],
            timeout_sec=30.0,
        ),
        ServiceSpec(
            name="proxy",
            service_dir=ctx.scan_dir / "proxy",
            log_file=ctx.proxy_log_file,
            ready_cmd=[python_exe, "-m", "s6.check_tcp_ready", "127.0.0.1", "3128"],
            timeout_sec=30.0,
        ),
    ]
    if ctx.mode == "dev":
        specs.extend(
            [
                ServiceSpec(
                    name="seaweedfs",
                    service_dir=ctx.scan_dir / "seaweedfs",
                    log_file=ctx.seaweed_log_file,
                    ready_cmd=[python_exe, "-m", "s6.check_seaweedfs_ready"],
                    timeout_sec=60.0,
                ),
                ServiceSpec(
                    name="test_target",
                    service_dir=ctx.scan_dir / "test_target",
                    log_file=ctx.test_target_log_file,
                    ready_cmd=[python_exe, "-m", "s6.check_http_ready", "http://127.0.0.1:18080/"],
                    timeout_sec=30.0,
                ),
            ]
        )

    specs.extend(
        [
            ServiceSpec(
                name="crawlerd",
                service_dir=ctx.crawlerd_service_dir,
                log_file=ctx.crawlerd_log_file,
                ready_cmd=[
                    python_exe,
                    "-m",
                    "s6.check_crawlerd_ready",
                    str(ctx.crawlerd_socket_path),
                ],
                timeout_sec=30.0,
            ),
            ServiceSpec(
                name="webshotd",
                service_dir=ctx.webshotd_service_dir,
                log_file=ctx.webshotd_log_file,
                ready_cmd=[python_exe, "-m", "s6.check_webshotd_ready", _webshotd_ready_url],
                timeout_sec=30.0,
            ),
        ]
    )
    return specs


def _enabled_services(ctx: RuntimeContext) -> list[str]:
    names = ["postgres", "proxy", "crawlerd", "webshotd"]
    if ctx.mode == "dev":
        names[2:2] = ["seaweedfs", "test_target"]
    return names


def _finish_script() -> str:
    return """#!/bin/sh
if [ "$1" -ne 0 ] && [ "$2" -ne 15 ]; then
  exit 125
fi
exit 0
"""


def _bootstrap_postgres(ctx: RuntimeContext) -> None:
    need_cmd("initdb")
    need_cmd("pg_ctl")
    need_cmd("psql")
    need_cmd("createdb")

    bootstrap_state = f"{ctx.postgres_capture_meta_db_name}\n{ctx.postgres_shared_state_db_name}\n"
    if (
        ctx.postgres_bootstrap_done_file.is_file()
        and ctx.postgres_bootstrap_done_file.read_text(encoding="utf-8") == bootstrap_state
    ):
        return

    was_initialized = ctx.postgres_data_dir.joinpath("PG_VERSION").is_file()
    ctx.postgres_data_dir.parent.mkdir(parents=True, exist_ok=True)
    ctx.postgres_run_dir.mkdir(parents=True, exist_ok=True)
    if not was_initialized:
        run(
            [
                "initdb",
                "-D",
                str(ctx.postgres_data_dir),
                "--username=postgres",
                "--auth=trust",
            ],
            timeout_sec=120.0,
        )
    run(
        [
            "pg_ctl",
            "-D",
            str(ctx.postgres_data_dir),
            "-l",
            str(ctx.postgres_bootstrap_log),
            "-o",
            (
                "-c timezone=UTC "
                "-c log_timezone=UTC "
                "-c listen_addresses=127.0.0.1 "
                f"-c unix_socket_directories={ctx.postgres_run_dir} "
                "-p 5432"
            ),
            "-w",
            "start",
        ],
        timeout_sec=120.0,
    )
    try:
        created_capture_meta = run(
            [
                "psql",
                "-h",
                "127.0.0.1",
                "-p",
                "5432",
                "-U",
                "postgres",
                "-d",
                "postgres",
                "-tAc",
                f"SELECT 1 FROM pg_database WHERE datname = '{ctx.postgres_capture_meta_db_name}'",
            ],
            capture=True,
            timeout_sec=30.0,
        ).stdout.strip()
        if created_capture_meta != "1":
            run(
                [
                    "createdb",
                    "-h",
                    "127.0.0.1",
                    "-p",
                    "5432",
                    "-U",
                    "postgres",
                    ctx.postgres_capture_meta_db_name,
                ],
                timeout_sec=30.0,
            )
        run(
            [
                "psql",
                "-h",
                "127.0.0.1",
                "-p",
                "5432",
                "-U",
                "postgres",
                "-d",
                ctx.postgres_capture_meta_db_name,
                "-f",
                str(ctx.repo_root / "webshotd/sql/schema/capture_meta_db.sql"),
            ],
            timeout_sec=30.0,
        )
        created_shared_state = run(
            [
                "psql",
                "-h",
                "127.0.0.1",
                "-p",
                "5432",
                "-U",
                "postgres",
                "-d",
                "postgres",
                "-tAc",
                f"SELECT 1 FROM pg_database WHERE datname = '{ctx.postgres_shared_state_db_name}'",
            ],
            capture=True,
            timeout_sec=30.0,
        ).stdout.strip()
        if created_shared_state != "1":
            run(
                [
                    "createdb",
                    "-h",
                    "127.0.0.1",
                    "-p",
                    "5432",
                    "-U",
                    "postgres",
                    ctx.postgres_shared_state_db_name,
                ],
                timeout_sec=30.0,
            )
        run(
            [
                "psql",
                "-h",
                "127.0.0.1",
                "-p",
                "5432",
                "-U",
                "postgres",
                "-d",
                ctx.postgres_shared_state_db_name,
                "-f",
                str(ctx.repo_root / "webshotd/sql/schema/shared_state_db.sql"),
            ],
            timeout_sec=30.0,
        )
        ctx.postgres_bootstrap_done_file.write_text(bootstrap_state, encoding="utf-8")
    finally:
        run(
            ["pg_ctl", "-D", str(ctx.postgres_data_dir), "-m", "fast", "-w", "stop"],
            timeout_sec=120.0,
        )


def _render_test_target_config(ctx: RuntimeContext) -> None:
    raw = (ctx.repo_root / "container/nginx_test.conf").read_text(encoding="utf-8")
    raw = raw.replace("listen 80;", "listen 18080;")
    raw = raw.replace("listen 443 ssl;", "listen 18443 ssl;")
    raw = raw.replace(
        "/etc/nginx/ssl/test_target.crt",
        str(ctx.repo_root / "container/compose/pki/test_target.crt"),
    )
    raw = raw.replace(
        "/etc/nginx/ssl/test_target.key",
        str(ctx.repo_root / "container/compose/pki/test_target.key"),
    )
    rendered = (
        "worker_processes 1;\n"
        "error_log stderr info;\n"
        f"pid {_shell_quote(ctx.test_target_dir / 'nginx.pid')};\n"
        "events {}\n"
        "http {\n"
        "    access_log /dev/stdout;\n\n"
        + "\n".join(f"    {line}" if line else "" for line in raw.splitlines())
        + "\n}\n"
    )
    _write_text(ctx.test_target_config_path, rendered)


def _render_proxy_runtime(ctx: RuntimeContext) -> None:
    need_cmd("mitmdump")
    ctx.proxy_confdir.mkdir(parents=True, exist_ok=True)
    if ctx.mode != "dev":
        return

    verify_paths = ssl.get_default_verify_paths()
    ca_file = verify_paths.cafile
    if ca_file is None:
        die("Failed to locate the default upstream CA bundle", exit_code=2)
    ca_path = Path(ca_file)
    if not ca_path.is_file():
        die(f"Default upstream CA bundle is missing: {ca_path}", exit_code=2)

    origin_ca_path = ctx.repo_root / "container/compose/pki/origin_ca.crt"
    _write_bytes(
        ctx.proxy_upstream_ca_file,
        ca_path.read_bytes().rstrip() + b"\n" + origin_ca_path.read_bytes(),
    )


def _wait_script(cmd: list[str]) -> str:
    return (
        f"while ! {_shell_quote(cmd[0])}"
        + "".join(f" {_shell_quote(part)}" for part in cmd[1:])
        + " >/dev/null 2>&1; do sleep 0.2; done\n"
    )


def _render_service_tree(ctx: RuntimeContext, *, cpu_limit: str) -> list[ServiceSpec]:
    ctx.scan_dir.mkdir(parents=True, exist_ok=True)
    for spec in _service_specs(ctx):
        (spec.service_dir / "data").mkdir(parents=True, exist_ok=True)
        _write_executable(spec.service_dir / "finish", _finish_script())

    python_exe = _shell_quote(sys.executable)
    repo_root = _shell_quote(ctx.repo_root)
    webshot_root = _shell_quote(ctx.repo_root / "webshotd")
    binary_path = _shell_quote(ctx.binary_path)
    config_vars_path = _shell_quote(ctx.config_vars_source)
    ld_library_path = _shell_quote(ctx.runtime_ld_library_path)
    cpu_limit_quoted = _shell_quote(cpu_limit)
    deploy_vcpu_limit = _shell_quote(ctx.deploy_vcpu_limit)

    postgres_service = ctx.scan_dir / "postgres"
    _write_executable(
        postgres_service / "data/check",
        "#!/bin/sh\nexec pg_isready -h 127.0.0.1 -p 5432 -U postgres\n",
    )
    _write_executable(
        postgres_service / "run",
        (
            "#!/bin/sh\n"
            f"exec >>{_shell_quote(ctx.postgres_log_file)} 2>&1\n"
            f"cd {repo_root}\n"
            f"exec postgres -D {_shell_quote(ctx.postgres_data_dir)} "
            f"-h 127.0.0.1 -k {_shell_quote(ctx.postgres_run_dir)} -p 5432 "
            "-c timezone=UTC -c log_timezone=UTC\n"
        ),
    )

    proxy_service = ctx.scan_dir / "proxy"
    _write_executable(
        proxy_service / "data/check",
        f"#!/bin/sh\nexec {python_exe} -m s6.check_tcp_ready 127.0.0.1 3128\n",
    )
    proxy_cmd: list[str | Path] = [
        "mitmdump",
        "--mode",
        "regular",
        "--listen-host",
        "127.0.0.1",
        "--listen-port",
        "3128",
        "--set",
        f"confdir={ctx.proxy_confdir}",
        "--set",
        "connection_strategy=lazy",
        "--set",
        "upstream_cert=false",
        "--set",
        "showhost=false",
        "--set",
        "validate_inbound_headers=true",
        "--set",
        f"webshot_mode={ctx.mode}",
        "--set",
        "webshot_denylist_url=http://127.0.0.1:8080/v1/denylist/check",
        "-s",
        ctx.repo_root / "s6/mitm_addon.py",
    ]
    if ctx.mode == "dev":
        proxy_cmd.extend(
            [
                "--set",
                f"ssl_verify_upstream_trusted_ca={ctx.proxy_upstream_ca_file}",
            ]
        )
    _write_executable(
        proxy_service / "run",
        (
            "#!/bin/sh\n"
            f"exec >>{_shell_quote(ctx.proxy_log_file)} 2>&1\n"
            f"cd {repo_root}\n"
            f"exec {_shell_join(proxy_cmd)}\n"
        ),
    )

    if ctx.mode == "dev":
        seaweed_service = ctx.scan_dir / "seaweedfs"
        _write_executable(
            seaweed_service / "data/check",
            f"#!/bin/sh\nexec {python_exe} -m s6.check_seaweedfs_ready\n",
        )
        _write_executable(
            seaweed_service / "run",
            (
                "#!/bin/sh\n"
                f"exec >>{_shell_quote(ctx.seaweed_log_file)} 2>&1\n"
                f"cd {repo_root}\n"
                "exec weed server -s3 -filer -ip.bind=0.0.0.0 "
                f"-dir={_shell_quote(ctx.seaweed_data_dir)} "
                "-volume.port=8082 -volume.port.grpc=18082 "
                "-master.volumeSizeLimitMB=32 -metricsPort=9324 "
                f"-s3.config={_shell_quote(ctx.repo_root / 'container/seaweed/s3_config.json')}\n"
            ),
        )

        test_target_service = ctx.scan_dir / "test_target"
        _write_executable(
            test_target_service / "data/check",
            f"#!/bin/sh\nexec {python_exe} -m s6.check_http_ready http://127.0.0.1:18080/\n",
        )
        _write_executable(
            test_target_service / "run",
            (
                "#!/bin/sh\n"
                f"exec >>{_shell_quote(ctx.test_target_log_file)} 2>&1\n"
                f"cd {repo_root}\n"
                f"exec nginx -c {_shell_quote(ctx.test_target_config_path)} "
                "-g 'daemon off;'\n"
            ),
        )

    _write_executable(
        ctx.crawlerd_service_dir / "data/check",
        (
            "#!/bin/sh\n"
            f"exec {python_exe} -m s6.check_crawlerd_ready "
            f"{_shell_quote(ctx.crawlerd_socket_path)}\n"
        ),
    )
    crawlerd_waits = _wait_script([sys.executable, "-m", "s6.check_tcp_ready", "127.0.0.1", "3128"])
    if ctx.mode == "dev":
        crawlerd_waits += _wait_script(
            [sys.executable, "-m", "s6.check_http_ready", "http://127.0.0.1:18080/"]
        )
    # Run crawlerd from the working tree so every mode sees the local build.
    need_cmd("node")
    node_path = shutil.which("node")
    assert node_path is not None
    crawlerd_run_command = _shell_join(
        [
            node_path,
            ctx.repo_root / "crawlerd" / "dist" / "src" / "server.js",
            "--socket-path",
            ctx.crawlerd_socket_path,
        ]
    )
    _write_executable(
        ctx.crawlerd_service_dir / "run",
        (
            "#!/bin/sh\n"
            f"exec >>{_shell_quote(ctx.crawlerd_log_file)} 2>&1\n"
            f"cd {repo_root}\n" + crawlerd_waits + "exec "
            f"env CPU_LIMIT={cpu_limit_quoted} DEPLOY_VCPU_LIMIT={deploy_vcpu_limit} "
            f"{crawlerd_run_command}\n"
        ),
    )

    _write_executable(
        ctx.webshotd_service_dir / "data/check",
        (
            "#!/bin/sh\n"
            f"exec {python_exe} -m s6.check_webshotd_ready "
            f"{_shell_quote(_webshotd_ready_url)}\n"
        ),
    )
    webshotd_waits = _wait_script(["pg_isready", "-h", "127.0.0.1", "-p", "5432", "-U", "postgres"])
    webshotd_waits += _wait_script(
        [sys.executable, "-m", "s6.check_tcp_ready", "127.0.0.1", "3128"]
    )
    if ctx.mode == "dev":
        webshotd_waits += _wait_script([sys.executable, "-m", "s6.check_seaweedfs_ready"])
    _write_executable(
        ctx.webshotd_service_dir / "run",
        (
            "#!/bin/sh\n"
            f"exec >>{_shell_quote(ctx.webshotd_log_file)} 2>&1\n"
            f"cd {webshot_root}\n" + webshotd_waits + "exec "
            f"env LD_LIBRARY_PATH={ld_library_path} CPU_LIMIT={cpu_limit_quoted} "
            f"DEPLOY_VCPU_LIMIT={deploy_vcpu_limit} "
            f"{binary_path} --config "
            f"{_shell_quote(ctx.repo_root / 'webshotd/config/static_config.yaml')} "
            f"--config_vars {config_vars_path}\n"
        ),
    )
    return _service_specs(ctx)


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
    for spec in _service_specs(ctx):
        if (
            run(
                ["s6-svok", str(spec.service_dir)], check=False, timeout_sec=_cmd_timeout_sec
            ).returncode
            != 0
        ):
            return False
    return True


def _stack_healthy(ctx: RuntimeContext) -> bool:
    if not _supervisor_running(ctx):
        return False
    for service in _service_specs(ctx):
        if run(service.ready_cmd, check=False, timeout_sec=_cmd_timeout_sec).returncode != 0:
            return False
    return True


def _start_supervisor(ctx: RuntimeContext, services: list[ServiceSpec]) -> None:
    need_cmd("s6-svscan")

    pid = _read_pid(ctx.svscan_pid_file)
    if pid is not None and _pid_is_running(pid):
        print("s6: already running")
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

    deadline = time.monotonic() + _start_timeout_sec
    while not _supervisor_running(ctx):
        if not _pid_is_running(proc.pid):
            die("s6: failed to start supervisor", exit_code=1)
        if time.monotonic() >= deadline:
            die("s6: timed out waiting for supervisors", exit_code=1)
        time.sleep(0.1)

    for service in services:
        _wait_ready(service)


def _wait_ready(service: ServiceSpec) -> None:
    deadline = time.monotonic() + service.timeout_sec
    while time.monotonic() < deadline:
        if run(service.ready_cmd, check=False, timeout_sec=_cmd_timeout_sec).returncode == 0:
            return
        time.sleep(_poll_interval_sec)

    tail = "\n".join(tail_lines(service.log_file, max_lines=40))
    detail = f"\n{tail}" if tail else ""
    die(f"Timed out waiting for {service.name} readiness{detail}", exit_code=1)


def _stop_supervisor(ctx: RuntimeContext) -> None:
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
        timeout_sec=_cmd_timeout_sec,
    )
    if proc.returncode != 0:
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


def _show_service_status(spec: ServiceSpec) -> None:
    need_cmd("s6-svstat")
    if (
        run(
            ["s6-svok", str(spec.service_dir)], check=False, timeout_sec=_cmd_timeout_sec
        ).returncode
        != 0
    ):
        print(f"{spec.name}: not supervised")
        return
    proc = run(["s6-svstat", str(spec.service_dir)], capture=True, timeout_sec=_cmd_timeout_sec)
    print(f"{spec.name}: {proc.stdout.strip()}")


def _spawn_logs(ctx: RuntimeContext) -> list[subprocess.Popen[str]]:
    need_cmd("tail")
    log_files = [str(spec.log_file) for spec in _service_specs(ctx)]
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


def _logs(ctx: RuntimeContext) -> None:
    procs = _spawn_logs(ctx)
    try:
        while True:
            if procs and all(proc.poll() is not None for proc in procs):
                return
            time.sleep(0.2)
    finally:
        _cleanup_processes(procs)


def _ensure_dev_bucket(ctx: RuntimeContext) -> None:
    ensure_s3_bucket_exists(
        secrets_path=ctx.repo_root / "webshotd/secret/test_secdist.json",
        endpoint="localhost:8333",
        bucket="webshot",
    )


def _prepare_runtime(ctx: RuntimeContext, *, cpu_limit: str) -> list[ServiceSpec]:
    ctx.state_dir.mkdir(parents=True, exist_ok=True)
    _bootstrap_postgres(ctx)
    _render_proxy_runtime(ctx)
    if ctx.mode == "dev":
        ctx.seaweed_data_dir.mkdir(parents=True, exist_ok=True)
        ctx.test_target_dir.mkdir(parents=True, exist_ok=True)
        _render_test_target_config(ctx)
    ctx.crawlerd_socket_path.parent.mkdir(parents=True, exist_ok=True)
    ctx.crawlerd_socket_path.unlink(missing_ok=True)
    return _render_service_tree(ctx, cpu_limit=cpu_limit)


def _up(ctx: RuntimeContext) -> None:
    if _supervisor_running(ctx):
        if not _stack_healthy(ctx):
            die("s6: stack already supervised but not healthy; run down first", exit_code=1)
        print("s6: already running")
        if ctx.mode == "dev":
            _ensure_dev_bucket(ctx)
        return

    cpu_limit = resolve_cpu_limit()
    services = _prepare_runtime(ctx, cpu_limit=cpu_limit)
    _start_supervisor(ctx, services)
    if ctx.mode == "dev":
        _ensure_dev_bucket(ctx)


def _down(ctx: RuntimeContext) -> None:
    _stop_supervisor(ctx)
    if ctx.state_dir.exists():
        shutil.rmtree(ctx.state_dir)


def _status(ctx: RuntimeContext) -> None:
    if not _supervisor_running(ctx):
        print("s6: not running")
        return
    for spec in _service_specs(ctx):
        _show_service_status(spec)


def _check(ctx: RuntimeContext) -> int:
    return 0 if _stack_healthy(ctx) else 1


def main() -> int:
    parser = argparse.ArgumentParser(prog="s6.runtime")
    parser.add_argument("action", choices=["up", "down", "status", "logs", "check"])
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
        elif args.action == "check":
            return _check(ctx)
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
