import asyncio
import os
import pathlib
import socket
import subprocess
import time
from urllib.parse import urlparse, urlunparse

import psycopg2
import psycopg2.extras
import pytest
import yaml
from pytest_userver import chaos
from testsuite.databases.pgsql import discover

from s6.s3_bucket import ensure_s3_bucket_exists

_S3_GATE_HOST = "localhost"
_SERVICE_PORT = 8080
_MONITOR_PORT = 8081

pytest_plugins = [
    "pytest_userver.plugins.core",
    "pytest_userver.plugins.postgresql",
    "pytest_userver.plugins.config",
    "pytest_userver.chaos",
    "helper.sql_loader",
]

psycopg2.extras.register_uuid()


def _require_cmake_cache_string(path: pathlib.Path, key: str) -> str:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError as e:
        raise RuntimeError(f"missing CMake cache: {path}") from e

    prefix = f"{key}:"
    for line in lines:
        if not line.startswith(prefix):
            continue
        _, value = line.split("=", 1)
        if value:
            return value
        break

    raise RuntimeError(f"missing CMake cache entry {key!r} in {path}")


@pytest.fixture(scope="session")
def service_port() -> int:
    return _SERVICE_PORT


@pytest.fixture(scope="session")
def monitor_port() -> int:
    return _MONITOR_PORT


@pytest.fixture(scope="session")
def pgsql_local(pgsql_local_create, service_source_dir: pathlib.Path):
    schemas = discover.find_schemas(
        None,
        [service_source_dir / "sql" / "schema"],
    )
    return pgsql_local_create(list(schemas.values()))


@pytest.fixture(scope="session")
async def pg_gate(pgsql_local):
    # Assume all test databases share the same host/port.
    any_conn = next(iter(pgsql_local.values()))
    uri = any_conn.get_uri()
    parsed = urlparse(uri)
    host = parsed.hostname or "localhost"
    port = parsed.port or 5432

    route = chaos.GateRoute(
        name="postgres",
        host_to_server=host,
        port_to_server=port,
    )
    loop = asyncio.get_event_loop()
    gate = chaos.TcpGate(route, loop)
    gate.start()
    try:
        yield gate
    finally:
        await gate.stop()


@pytest.fixture(scope="session")
async def s3_gate(s3_gate_port):
    # Proxy between service and local S3 (SeaweedFS) through a per-run local port.
    route = chaos.GateRoute(
        name="s3",
        host_to_server=_S3_GATE_HOST,
        port_to_server=8333,
        host_for_client=_S3_GATE_HOST,
        port_for_client=s3_gate_port,
    )
    loop = asyncio.get_event_loop()
    gate = chaos.TcpGate(route, loop)
    gate.start()
    try:
        yield gate
    finally:
        await gate.stop()


@pytest.fixture(scope="session")
def s3_gate_port(choose_free_port):
    return choose_free_port(8334)


@pytest.fixture
async def pg_gate_ready(pg_gate):
    await pg_gate.to_server_pass()
    await pg_gate.to_client_pass()
    pg_gate.start_accepting()
    yield pg_gate


@pytest.fixture
async def s3_gate_ready(s3_gate, service_source_dir: pathlib.Path):
    await s3_gate.to_server_pass()
    await s3_gate.to_client_pass()
    s3_gate.start_accepting()
    secrets_path = service_source_dir / "secret" / "test_secdist.json"
    ensure_s3_bucket_exists(secrets_path=secrets_path, endpoint="localhost:8333", bucket="webshot")
    yield s3_gate


@pytest.fixture(scope="session")
def userver_pg_config(pgsql_local, pg_gate):
    db_uri = {name: conn.get_uri() for name, conn in pgsql_local.items()}
    gate_host, gate_port = pg_gate.get_sockname_for_clients()

    def _patch(config_yaml, _config_vars):
        components = config_yaml["components_manager"]["components"]
        for component_name in ("capture_meta_db", "shared_state_db"):
            conninfo = db_uri[component_name]
            parsed = urlparse(conninfo)
            netloc = parsed.netloc
            if "@" in netloc:
                auth, _ = netloc.rsplit("@", 1)
                netloc = f"{auth}@{gate_host}:{gate_port}"
            else:
                netloc = f"{gate_host}:{gate_port}"
            updated = parsed._replace(netloc=netloc)
            component = components[component_name]
            component["dbconnection"] = urlunparse(updated)
            component.pop("dbalias", None)

    return _patch


@pytest.fixture(scope="session")
def service_secdist_path(service_source_dir: pathlib.Path) -> pathlib.Path:
    return service_source_dir / "secret" / "test_secdist.json"


@pytest.fixture(scope="session")
def service_env(service_source_dir: pathlib.Path):
    return {
        "LSAN_OPTIONS": f"suppressions={service_source_dir}/lsan.supp",
    }


@pytest.fixture(scope="session")
def allowed_url_prefixes_extra(s3_gate_port):
    # Permit S3 uploads to the chaos gate in front of the local SeaweedFS endpoint.
    return [f"http://{_S3_GATE_HOST}:{s3_gate_port}/", "http://localhost/run"]


@pytest.fixture(scope="session")
def patch_s3_config(s3_gate_port):
    def _patch(config_yaml, _config_vars):
        components = config_yaml["components_manager"]["components"]
        cfg = components["config"]
        cfg["s3_endpoint"] = f"http://{_S3_GATE_HOST}:{s3_gate_port}"
        cfg["s3_timeout_ms"] = 5000

    return _patch


@pytest.fixture(scope="session")
def patch_crawlerd_config(crawlerd_socket_path: pathlib.Path):
    def _patch(_config_yaml, config_vars):
        config_vars["crawlerd_socket_path"] = str(crawlerd_socket_path)

    return _patch


@pytest.fixture(scope="session")
def service_config_path_temp(
    service_tmpdir,
    _service_config_hooked,
    service_binary: pathlib.Path,
    service_source_dir: pathlib.Path,
) -> pathlib.Path:
    dst_path = service_tmpdir / "config.yaml"
    config_yaml = dict(_service_config_hooked.config_yaml)
    config_vars = dict(_service_config_hooked.config_vars)
    cmake_cache_path = service_binary.parent / "CMakeCache.txt"
    config_vars["rapidoc_assets_dir"] = _require_cmake_cache_string(
        cmake_cache_path, "WEBSHOT_RAPIDOC_ASSETS_DIR"
    )
    config_vars["openapi_dir"] = str(service_source_dir.parent / "schema")

    components = config_yaml["components_manager"]["components"]
    if "http-client" in components:
        http_client = components["http-client"] or {}
        http_client["testsuite-timeout"] = "20s"

    if not config_vars:
        config_yaml.pop("config_vars", None)
    else:
        config_vars_path = service_tmpdir / "config_vars.yaml"
        config_vars_path.write_text(yaml.dump(config_vars))
        config_yaml["config_vars"] = str(config_vars_path)

    dst_path.write_text(yaml.dump(config_yaml))
    return dst_path


@pytest.fixture(scope="session")
def crawlerd_runtime_dir() -> pathlib.Path:
    runtime_dir = pathlib.Path("/tmp") / f"webshotd-tests-{os.getuid()}-{os.getpid()}"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    return runtime_dir


@pytest.fixture(scope="session")
def crawlerd_socket_path(crawlerd_runtime_dir: pathlib.Path):
    return crawlerd_runtime_dir / "crawlerd.sock"


def _crawlerd_ready(socket_path: pathlib.Path) -> bool:
    if not socket_path.exists():
        return False

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(1.0)
            sock.connect(str(socket_path))
            sock.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            return b"200" in sock.recv(4096)
    except OSError:
        return False


@pytest.fixture(scope="session")
def crawlerd_process(crawlerd_socket_path: pathlib.Path, service_source_dir: pathlib.Path):
    repo_root = service_source_dir.parent
    crawler_root = repo_root / "crawlerd"
    log_dir = crawlerd_socket_path.parent
    log_path = log_dir / "crawlerd.log"

    log_dir.mkdir(parents=True, exist_ok=True)
    crawlerd_socket_path.unlink(missing_ok=True)

    with log_path.open("wb") as log:
        subprocess.run(
            ["npm", "run", "build"],
            cwd=crawler_root,
            stdout=log,
            stderr=log,
            check=True,
        )
        process = subprocess.Popen(
            ["node", "dist/src/server.js", "--socket-path", str(crawlerd_socket_path)],
            cwd=crawler_root,
            stdout=log,
            stderr=log,
        )

    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"crawlerd exited early with code {process.returncode}")
        if _crawlerd_ready(crawlerd_socket_path):
            break
        time.sleep(0.2)
    else:
        raise RuntimeError(f"crawlerd did not become ready; see {log_path}")

    try:
        yield process
    finally:
        process.terminate()
        try:
            process.wait(timeout=10.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10.0)


@pytest.fixture
def crawlerd_ready(crawlerd_process):
    return crawlerd_process


@pytest.fixture
def extra_client_deps(pg_gate_ready, s3_gate_ready, crawlerd_ready):
    # Ensure gates are reset and crawlerd is running before service_client is created.
    return [pg_gate_ready, s3_gate_ready, crawlerd_ready]


USERVER_CONFIG_HOOKS = ["patch_s3_config", "patch_crawlerd_config"]
