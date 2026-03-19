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
from typing import cast
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
_logs_wait_timeout_sec = 5.0
_webshotd_ready_url = "http://127.0.0.1:8081/service/monitor?format=json"


@dataclass(frozen=True)
class ServiceSpec:
    name: str
    service_dir: Path
    log_file: Path
    ready_cmd: list[str]
    timeout_sec: float


@dataclass(frozen=True)
class RuntimeStateContext:
    mode: str
    repo_root: Path
    state_dir: Path
    scan_dir: Path
    svscan_pid_file: Path


@dataclass(frozen=True)
class RuntimeInspectContext(RuntimeStateContext):
    service_profile: str
    postgres_log_file: Path
    proxy_log_file: Path
    seaweed_log_file: Path
    test_target_log_file: Path
    webshotd_service_dir: Path
    webshotd_log_file: Path


@dataclass(frozen=True)
class RuntimeUpContext(RuntimeInspectContext):
    binary_path: Path
    config_vars_source: Path
    runtime_ld_library_path: str
    deploy_vcpu_limit: str
    postgres_data_dir: Path
    postgres_run_dir: Path
    postgres_bootstrap_done_file: Path
    postgres_bootstrap_log: Path
    postgres_capture_meta_db_name: str
    postgres_shared_state_db_name: str
    proxy_confdir: Path
    proxy_upstream_ca_file: Path
    seaweed_data_dir: Path
    test_target_dir: Path


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


def _require_cmake_cache_string(path: Path, key: str) -> str:
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            prefix = f"{key}:"
            if not line.startswith(prefix):
                continue
            _, value = line.split("=", 1)
            if value:
                return value
            break
    except FileNotFoundError as e:
        raise ToolError(message=f"Missing CMake cache: {path}", exit_code=2) from e

    die(f"Missing required CMake cache entry '{key}' in {path}", exit_code=2)


def _require_yaml_path(raw: dict[str, object], key: str, *, source: Path) -> Path:
    value = raw.get(key)
    if not isinstance(value, str):
        die(f"Missing required config var '{key}' in {source}", exit_code=2)
    if not value:
        die(f"Missing required config var '{key}' in {source}", exit_code=2)
    return Path(cast(str, value))


def _require_yaml_string(raw: dict[str, object], key: str, *, source: Path) -> str:
    value = raw.get(key)
    if not isinstance(value, str):
        die(f"Missing required config var '{key}' in {source}", exit_code=2)
    if not value:
        die(f"Missing required config var '{key}' in {source}", exit_code=2)
    return cast(str, value)


def _database_name_from_dsn(dsn: str, *, key: str, source: Path) -> str:
    parsed = urlsplit(dsn)
    db_name = parsed.path.lstrip("/")
    if not db_name:
        die(f"Config var '{key}' in {source} must include a database name", exit_code=2)
    return db_name


def _fixed_state_dir(mode: str) -> Path:
    return Path("/tmp/webshot") / mode


def _build_state_context(*, mode: str) -> RuntimeStateContext:
    repo_root = _repo_root()
    state_dir = _fixed_state_dir(mode)
    return RuntimeStateContext(
        mode=mode,
        repo_root=repo_root,
        state_dir=state_dir,
        scan_dir=state_dir / "s6-scan",
        svscan_pid_file=state_dir / "s6-svscan.pid",
    )


def _build_inspect_context(
    *,
    mode: str,
    service_profile: str,
) -> RuntimeInspectContext:
    if service_profile == "test_infra" and mode != "dev":
        die("service profile 'test_infra' requires --mode dev", exit_code=2)

    state_ctx = _build_state_context(mode=mode)
    return RuntimeInspectContext(
        mode=state_ctx.mode,
        repo_root=state_ctx.repo_root,
        state_dir=state_ctx.state_dir,
        scan_dir=state_ctx.scan_dir,
        svscan_pid_file=state_ctx.svscan_pid_file,
        service_profile=service_profile,
        postgres_log_file=state_ctx.state_dir / "postgres.log",
        proxy_log_file=state_ctx.state_dir / "mitmproxy.log",
        seaweed_log_file=state_ctx.state_dir / "seaweedfs.log",
        test_target_log_file=state_ctx.state_dir / "test-target.log",
        webshotd_service_dir=state_ctx.scan_dir / "webshotd",
        webshotd_log_file=state_ctx.state_dir / "webshotd.log",
    )


def _build_up_context(
    *,
    mode: str,
    service_profile: str,
    binary_path: str,
    config_vars_source: str,
    runtime_ld_library_path: str,
) -> RuntimeUpContext:
    inspect_ctx = _build_inspect_context(mode=mode, service_profile=service_profile)
    config_path = Path(config_vars_source)
    raw_vars = _read_yaml(config_path)
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
    return RuntimeUpContext(
        mode=inspect_ctx.mode,
        repo_root=inspect_ctx.repo_root,
        state_dir=inspect_ctx.state_dir,
        scan_dir=inspect_ctx.scan_dir,
        svscan_pid_file=inspect_ctx.svscan_pid_file,
        service_profile=service_profile,
        postgres_log_file=inspect_ctx.postgres_log_file,
        proxy_log_file=inspect_ctx.proxy_log_file,
        seaweed_log_file=inspect_ctx.seaweed_log_file,
        test_target_log_file=inspect_ctx.test_target_log_file,
        webshotd_service_dir=inspect_ctx.webshotd_service_dir,
        webshotd_log_file=inspect_ctx.webshotd_log_file,
        binary_path=Path(binary_path),
        config_vars_source=config_path,
        runtime_ld_library_path=runtime_ld_library_path,
        deploy_vcpu_limit=os.environ.get("DEPLOY_VCPU_LIMIT", ""),
        postgres_data_dir=inspect_ctx.state_dir / "postgres" / "data",
        postgres_run_dir=inspect_ctx.state_dir / "postgres" / "run",
        postgres_bootstrap_done_file=inspect_ctx.state_dir / "postgres" / ".bootstrap-complete",
        postgres_bootstrap_log=inspect_ctx.state_dir / "postgres-bootstrap.log",
        postgres_capture_meta_db_name=capture_meta_db_name,
        postgres_shared_state_db_name=shared_state_db_name,
        proxy_confdir=inspect_ctx.state_dir / "mitmproxy" / "confdir",
        proxy_upstream_ca_file=inspect_ctx.state_dir / "mitmproxy" / "upstream-ca.pem",
        seaweed_data_dir=inspect_ctx.state_dir / "seaweed",
        test_target_dir=inspect_ctx.state_dir / "test-target",
    )


def _service_specs(ctx: RuntimeInspectContext) -> list[ServiceSpec]:
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

    if ctx.service_profile == "full":
        specs.extend(
            [
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


def _finish_script() -> str:
    return """#!/bin/sh
if [ "$1" -ne 0 ] && [ "$2" -ne 15 ]; then
  exit 125
fi
exit 0
"""


def _bootstrap_postgres(ctx: RuntimeUpContext) -> None:
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


def _render_proxy_runtime(ctx: RuntimeUpContext) -> None:
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

    origin_ca_path = ctx.repo_root / "test/pki/origin_ca.crt"
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


def _render_service_tree(ctx: RuntimeUpContext, *, cpu_limit: str) -> list[ServiceSpec]:
    active_specs = _service_specs(ctx)
    active_names = {spec.name for spec in active_specs}
    ctx.scan_dir.mkdir(parents=True, exist_ok=True)
    for spec in active_specs:
        (spec.service_dir / "data").mkdir(parents=True, exist_ok=True)
        _write_executable(spec.service_dir / "finish", _finish_script())

    python_exe = _shell_quote(sys.executable)
    repo_root = _shell_quote(ctx.repo_root)
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
                f"-s3.config={_shell_quote(ctx.repo_root / 'seaweedfs/s3_config.json')}\n"
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
                "exec "
                + _shell_join(
                    [
                        "nginx",
                        "-e",
                        "stderr",
                        "-p",
                        ctx.repo_root,
                        "-c",
                        ctx.repo_root / "nginx" / "nginx_test.conf",
                        "-g",
                        f"daemon off; pid {ctx.test_target_dir / 'nginx.pid'};",
                    ]
                )
                + "\n"
            ),
        )

    if "webshotd" in active_names:
        webshot_root = _shell_quote(ctx.repo_root / "webshotd")
        binary_path = _shell_quote(ctx.binary_path)
        config_vars_path = _shell_quote(ctx.config_vars_source)
        ld_library_path = _shell_quote(ctx.runtime_ld_library_path)
        cmake_cache_path = ctx.binary_path.parent / "CMakeCache.txt"
        rapidoc_assets_dir = _require_cmake_cache_string(
            cmake_cache_path, "WEBSHOT_RAPIDOC_ASSETS_DIR"
        )
        config_vars_override_path = ctx.state_dir / "webshotd-config-vars-override.yaml"
        _write_text(
            config_vars_override_path,
            yaml.safe_dump(
                {
                    "rapidoc_assets_dir": rapidoc_assets_dir,
                    "openapi_dir": str(ctx.repo_root / "schema"),
                    "state_dir": str(ctx.state_dir / "webshotd"),
                },
                sort_keys=True,
            ),
        )
        config_vars_override_path_quoted = _shell_quote(config_vars_override_path)
        _write_executable(
            ctx.webshotd_service_dir / "data/check",
            (
                "#!/bin/sh\n"
                f"exec {python_exe} -m s6.check_webshotd_ready "
                f"{_shell_quote(_webshotd_ready_url)}\n"
            ),
        )
        webshotd_waits = _wait_script(
            ["pg_isready", "-h", "127.0.0.1", "-p", "5432", "-U", "postgres"]
        )
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
                f"--config_vars {config_vars_path} "
                f"--config_vars_override {config_vars_override_path_quoted}\n"
            ),
        )
    return active_specs


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


def _supervisor_running(ctx: RuntimeInspectContext) -> bool:
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


def _supervisor_matches_profile(ctx: RuntimeInspectContext) -> bool:
    need_cmd("s6-svok")
    active_names = {spec.name for spec in _service_specs(ctx)}
    known_service_dirs = {
        "postgres": ctx.scan_dir / "postgres",
        "proxy": ctx.scan_dir / "proxy",
        "seaweedfs": ctx.scan_dir / "seaweedfs",
        "test_target": ctx.scan_dir / "test_target",
        "webshotd": ctx.webshotd_service_dir,
    }
    for name, service_dir in known_service_dirs.items():
        supervised = (
            run(["s6-svok", str(service_dir)], check=False, timeout_sec=_cmd_timeout_sec).returncode
            == 0
        )
        if name in active_names:
            if not supervised:
                return False
            continue
        if supervised:
            return False
    return True


def _stack_healthy(ctx: RuntimeInspectContext) -> bool:
    if not _supervisor_running(ctx):
        return False
    for service in _service_specs(ctx):
        if run(service.ready_cmd, check=False, timeout_sec=_cmd_timeout_sec).returncode != 0:
            return False
    return True


def _start_supervisor(ctx: RuntimeUpContext, services: list[ServiceSpec]) -> None:
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


def _stop_supervisor(ctx: RuntimeStateContext) -> None:
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


def _up_task_name(ctx: RuntimeStateContext) -> str:
    return f"webshot:{ctx.mode}Up"


def _wait_for_log_files(ctx: RuntimeInspectContext) -> list[Path]:
    deadline = time.monotonic() + _logs_wait_timeout_sec
    missing = [spec.log_file for spec in _service_specs(ctx) if not spec.log_file.is_file()]
    while missing and time.monotonic() < deadline:
        time.sleep(_poll_interval_sec)
        missing = [path for path in missing if not path.is_file()]
    return missing


def _require_logs_ready(ctx: RuntimeInspectContext) -> list[Path]:
    if not _supervisor_running(ctx):
        die(
            f"s6: not running; run {_up_task_name(ctx)} first before reading logs",
            exit_code=1,
        )

    missing = _wait_for_log_files(ctx)
    if missing:
        missing_paths = "\n".join(str(path) for path in missing)
        die(
            f"s6: missing expected log files while supervised:\n{missing_paths}",
            exit_code=1,
        )
    return [spec.log_file for spec in _service_specs(ctx)]


def _spawn_logs(ctx: RuntimeInspectContext) -> list[subprocess.Popen[str]]:
    need_cmd("tail")
    log_files = [str(path) for path in _require_logs_ready(ctx)]
    print(f"s6: attaching {ctx.mode} logs")
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


def _logs(ctx: RuntimeInspectContext) -> None:
    procs = _spawn_logs(ctx)
    try:
        while True:
            if procs and all(proc.poll() is not None for proc in procs):
                return
            time.sleep(0.2)
    finally:
        _cleanup_processes(procs)


def _ensure_dev_bucket(ctx: RuntimeStateContext) -> None:
    ensure_s3_bucket_exists(
        secrets_path=ctx.repo_root / "webshotd/secret/test_secdist.json",
        s3_url="localhost:8333",
        bucket="webshot",
    )


def _prepare_runtime(ctx: RuntimeUpContext, *, cpu_limit: str) -> list[ServiceSpec]:
    ctx.state_dir.mkdir(parents=True, exist_ok=True)
    _bootstrap_postgres(ctx)
    _render_proxy_runtime(ctx)
    if ctx.scan_dir.exists():
        shutil.rmtree(ctx.scan_dir)
    if ctx.mode == "dev":
        ctx.seaweed_data_dir.mkdir(parents=True, exist_ok=True)
        ctx.test_target_dir.mkdir(parents=True, exist_ok=True)
    return _render_service_tree(ctx, cpu_limit=cpu_limit)


def _up(ctx: RuntimeUpContext) -> None:
    if _supervisor_running(ctx):
        if not _supervisor_matches_profile(ctx):
            die(
                "s6: stack already supervised with a different service profile; run down first",
                exit_code=1,
            )
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


def _down(ctx: RuntimeStateContext) -> None:
    _stop_supervisor(ctx)
    if ctx.state_dir.exists():
        shutil.rmtree(ctx.state_dir)


def _status(ctx: RuntimeInspectContext) -> None:
    if not _supervisor_running(ctx):
        print("s6: not running")
        return
    for spec in _service_specs(ctx):
        _show_service_status(spec)


def _check(ctx: RuntimeInspectContext) -> int:
    return 0 if _stack_healthy(ctx) else 1


def _add_mode_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--mode", required=True, choices=["dev", "prodlike"])


def _add_service_profile_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--service-profile", choices=["full", "test_infra"], default="full")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="s6.runtime")
    subparsers = parser.add_subparsers(dest="action", required=True)

    up_parser = subparsers.add_parser("up")
    _add_mode_argument(up_parser)
    _add_service_profile_argument(up_parser)
    up_parser.add_argument("--binary-path", required=True)
    up_parser.add_argument("--config-vars-source", required=True)
    up_parser.add_argument("--runtime-ld-library-path", required=True)

    _add_mode_argument(subparsers.add_parser("down"))

    for action in ["status", "logs", "check"]:
        action_parser = subparsers.add_parser(action)
        _add_mode_argument(action_parser)
        _add_service_profile_argument(action_parser)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    try:
        if args.action == "up":
            ctx = _build_up_context(
                mode=args.mode,
                service_profile=args.service_profile,
                binary_path=args.binary_path,
                config_vars_source=args.config_vars_source,
                runtime_ld_library_path=args.runtime_ld_library_path,
            )
            _up(ctx)
        elif args.action == "down":
            ctx = _build_state_context(mode=args.mode)
            _down(ctx)
        elif args.action == "status":
            ctx = _build_inspect_context(mode=args.mode, service_profile=args.service_profile)
            _status(ctx)
        elif args.action == "logs":
            ctx = _build_inspect_context(mode=args.mode, service_profile=args.service_profile)
            _logs(ctx)
        elif args.action == "check":
            ctx = _build_inspect_context(mode=args.mode, service_profile=args.service_profile)
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
