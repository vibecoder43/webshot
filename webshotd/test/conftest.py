import asyncio
import pathlib
import shutil
import tempfile
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
_DEV_STATE_DIR = pathlib.Path("/tmp/webshot/dev")
_DEV_WEBSHOTD_STATE_DIR = _DEV_STATE_DIR / "webshotd"
_DEV_TRUSTED_NSSDB_DIR = _DEV_WEBSHOTD_STATE_DIR / "test_pki" / "chromium_nssdb"
_TESTSUITE_STATE_ROOT = pathlib.Path("/tmp/webshot/testsuite")

pytest_plugins = [
    "pytest_userver.plugins.core",
    "pytest_userver.plugins.postgresql",
    "pytest_userver.plugins.config",
    "pytest_userver.chaos",
    "helper.sql_loader",
    "helper.coarse_profile",
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


def _require_service_binary_path(pytestconfig) -> pathlib.Path:
    binary = getattr(pytestconfig.option, "service_binary", None)
    if not binary:
        raise RuntimeError("--service-binary must be set for testsuite runs")
    return pathlib.Path(binary).resolve()


def _prepare_testsuite_webshotd_state_dir() -> pathlib.Path:
    if not _DEV_TRUSTED_NSSDB_DIR.is_dir():
        raise RuntimeError(f"missing runtime-generated Chromium NSS DB: {_DEV_TRUSTED_NSSDB_DIR}")

    _TESTSUITE_STATE_ROOT.mkdir(parents=True, exist_ok=True)
    state_dir = pathlib.Path(tempfile.mkdtemp(prefix="ws-", dir=_TESTSUITE_STATE_ROOT))

    trusted_nssdb_dir = state_dir / "test_pki" / "chromium_nssdb"
    trusted_nssdb_dir.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(_DEV_TRUSTED_NSSDB_DIR, trusted_nssdb_dir)
    return state_dir


@pytest.fixture(scope="session")
def service_port() -> int:
    return _SERVICE_PORT


@pytest.fixture(scope="session")
def monitor_port() -> int:
    return _MONITOR_PORT


@pytest.fixture(scope="session")
def service_binary(pytestconfig) -> pathlib.Path:
    return _require_service_binary_path(pytestconfig)


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
    ensure_s3_bucket_exists(secrets_path=secrets_path, s3_url="localhost:8333", bucket="webshot")
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
    # Permit S3 uploads through the local chaos gate and Chromium devtools
    # loopback URLs reached over the Unix socket transport in tests.
    return [
        f"http://{_S3_GATE_HOST}:{s3_gate_port}/",
        "http://localhost/",
        "ws://localhost/",
    ]


@pytest.fixture(scope="session")
def patch_s3_config(s3_gate_port):
    def _patch(config_yaml, _config_vars):
        components = config_yaml["components_manager"]["components"]
        cfg = components["config"]
        cfg["s3_endpoint"] = f"http://{_S3_GATE_HOST}:{s3_gate_port}"
        cfg["s3_timeout_ms"] = 3000

    return _patch


@pytest.fixture(scope="session")
def service_config_path_temp(
    service_tmpdir,
    _service_config_hooked,
    service_binary: pathlib.Path,
    service_source_dir: pathlib.Path,
) -> pathlib.Path:
    dst_path = service_tmpdir / "config.yaml"
    state_dir = _prepare_testsuite_webshotd_state_dir()
    config_yaml = dict(_service_config_hooked.config_yaml)
    config_vars = dict(_service_config_hooked.config_vars)
    cmake_cache_path = service_binary.parent / "CMakeCache.txt"
    config_vars["rapidoc_assets_dir"] = _require_cmake_cache_string(
        cmake_cache_path, "WEBSHOT_RAPIDOC_ASSETS_DIR"
    )
    config_vars["openapi_dir"] = str(service_source_dir.parent / "schema")
    config_vars["web_ui_dir"] = str(service_binary.parent / "web_ui")
    config_vars["state_dir"] = str(state_dir)

    components = config_yaml["components_manager"]["components"]
    http_client_core = components["http-client-core"]
    if http_client_core is None:
        raise RuntimeError("http-client-core component config must be present")
    http_client_core["testsuite-timeout"] = "5s"

    if not config_vars:
        config_yaml.pop("config_vars", None)
    else:
        config_vars_path = service_tmpdir / "config_vars.yaml"
        config_vars_path.write_text(yaml.dump(config_vars))
        config_yaml["config_vars"] = str(config_vars_path)

    dst_path.write_text(yaml.dump(config_yaml))
    return dst_path


@pytest.fixture
def extra_client_deps(pg_gate_ready, s3_gate_ready):
    return [pg_gate_ready, s3_gate_ready]


USERVER_CONFIG_HOOKS = ["patch_s3_config"]
