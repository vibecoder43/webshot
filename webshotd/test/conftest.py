import asyncio
import pathlib
import shutil
import tempfile
from urllib.parse import urlparse, urlunparse

import psycopg2
import psycopg2.extras
import pytest
import yaml
from helper.s3_gate_config import set_s3_gate_port
from pytest_userver import chaos
from testsuite.databases.pgsql import discover

from s6.runtime_context import runtime_layout_from_binary
from s6.s3_bucket import ensure_s3_bucket_exists
from s6.userver_task_processors import task_processor_config_vars

_S3_GATE_HOST = "127.0.0.1"
_TESTSUITE_S3_TIMEOUT_MS = 2000
_DEV_RUNTIME_STATE_ROOT = pathlib.Path("/tmp/webshot/dev")
_DEV_WEBSHOTD_TEST_PKI_DIR = pathlib.Path("/tmp/webshot/dev/webshotd/test_pki")
_TESTSUITE_STATE_ROOT = pathlib.Path("/tmp/webshot/testsuite")

pytest_plugins = [
    "pytest_userver.plugins.core",
    "pytest_userver.plugins.postgresql",
    "pytest_userver.plugins.config",
    "pytest_userver.chaos",
    "helper.sql_loader",
    "helper.capture_flow",
    "helper.coarse_profile",
]

psycopg2.extras.register_uuid()


def pytest_collection_modifyitems(items):
    normal_items = []
    chaos_s3_items = []

    for item in items:
        path = pathlib.Path(str(item.fspath))
        if path.name == "chaos_s3.py":
            chaos_s3_items.append(item)
        else:
            normal_items.append(item)

    items[:] = normal_items + chaos_s3_items


def _testsuite_worker_prefix(worker_id: str) -> str:
    return "master" if worker_id == "master" else worker_id


def _pgsql_service_name(worker_id: str) -> str | None:
    if worker_id == "master":
        return None
    return f"webshot_{_testsuite_worker_prefix(worker_id)}"


def _require_service_binary_path(pytestconfig) -> pathlib.Path:
    binary = getattr(pytestconfig.option, "service_binary", None)
    if not binary:
        raise RuntimeError("--service-binary must be set for testsuite runs")
    return pathlib.Path(binary).resolve()


def _require_service_listener_port(config_yaml) -> int:
    port = config_yaml["components_manager"]["components"]["server"]["listener"]["port"]
    if not isinstance(port, int):
        raise RuntimeError(f"service listener port must be resolved before testsuite run: {port!r}")
    return port


def _prepare_testsuite_webshotd_state_dir(
    service_binary: pathlib.Path, service_source_dir: pathlib.Path, worker_id: str
) -> pathlib.Path:
    del service_binary
    del service_source_dir

    if not _DEV_WEBSHOTD_TEST_PKI_DIR.is_dir():
        raise RuntimeError(f"missing runtime-generated test PKI: {_DEV_WEBSHOTD_TEST_PKI_DIR}")

    _TESTSUITE_STATE_ROOT.mkdir(parents=True, exist_ok=True)
    state_dir = pathlib.Path(
        tempfile.mkdtemp(
            prefix=f"ws-{_testsuite_worker_prefix(worker_id)}-",
            dir=_TESTSUITE_STATE_ROOT,
        )
    )
    target_test_pki_dir = state_dir / "test_pki"
    shutil.copytree(_DEV_WEBSHOTD_TEST_PKI_DIR, target_test_pki_dir)
    return state_dir


@pytest.fixture(scope="session")
def service_binary(pytestconfig) -> pathlib.Path:
    return _require_service_binary_path(pytestconfig)


@pytest.fixture(scope="session")
def testsuite_webshotd_state_dir(
    service_binary: pathlib.Path, service_source_dir: pathlib.Path, worker_id: str
) -> pathlib.Path:
    return _prepare_testsuite_webshotd_state_dir(service_binary, service_source_dir, worker_id)


@pytest.fixture(scope="session")
def test_target_payload_dir() -> pathlib.Path:
    payload_dir = _DEV_RUNTIME_STATE_ROOT / "nginx_payloads"
    payload_dir.mkdir(parents=True, exist_ok=True)
    return payload_dir


@pytest.fixture(scope="session")
def pgsql_local(pgsql_local_create, service_source_dir: pathlib.Path, worker_id: str):
    schemas = discover.find_schemas(
        _pgsql_service_name(worker_id),
        [service_source_dir / "sql" / "schema"],
    )
    return pgsql_local_create(list(schemas.values()))


@pytest.fixture(scope="session")
async def pg_gate(pgsql_local, worker_id: str):
    # Assume all test databases share the same host/port.
    any_conn = next(iter(pgsql_local.values()))
    uri = any_conn.get_uri()
    parsed = urlparse(uri)
    host = parsed.hostname or "localhost"
    port = parsed.port or 5432

    route = chaos.GateRoute(
        name=f"postgres-{_testsuite_worker_prefix(worker_id)}",
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
async def s3_gate(s3_gate_port, worker_id: str):
    # Proxy between service and local S3 (SeaweedFS) through a per-run local port.
    route = chaos.GateRoute(
        name=f"s3-{_testsuite_worker_prefix(worker_id)}",
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
    port = choose_free_port(8334)
    set_s3_gate_port(port)
    return port


@pytest.fixture(scope="session")
def s3_bucket_name(worker_id: str) -> str:
    if worker_id == "master":
        return "webshot"
    return f"webshot-{_testsuite_worker_prefix(worker_id)}"


@pytest.fixture(scope="session")
def s3_bucket_ready(service_source_dir: pathlib.Path, s3_bucket_name: str) -> None:
    secrets_path = service_source_dir / "secret" / "test_secdist.json"
    ensure_s3_bucket_exists(
        secrets_path=secrets_path,
        s3_url="127.0.0.1:8333",
        bucket=s3_bucket_name,
    )


@pytest.fixture
async def pg_gate_ready(pg_gate):
    await pg_gate.to_server_pass()
    await pg_gate.to_client_pass()
    pg_gate.start_accepting()
    yield pg_gate


@pytest.fixture
async def s3_gate_ready(s3_gate, s3_bucket_ready):
    await s3_gate.to_server_pass()
    await s3_gate.to_client_pass()
    s3_gate.start_accepting()
    try:
        yield s3_gate
    finally:
        await s3_gate.to_server_pass()
        await s3_gate.to_client_pass()
        s3_gate.start_accepting()


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
    # Permit direct local S3, S3 through the local chaos gate, and Chromium devtools
    # loopback URLs reached over the Unix socket transport in tests.
    return [
        "http://127.0.0.1:8333/",
        f"http://{_S3_GATE_HOST}:{s3_gate_port}/",
        "http://localhost/",
        "ws://localhost/",
    ]


@pytest.fixture(scope="session")
def patch_s3_config(s3_gate_port):
    del s3_gate_port

    def _patch(config_yaml, _config_vars):
        components = config_yaml["components_manager"]["components"]
        cfg = components["config"]
        cfg["s3_timeout_ms"] = _TESTSUITE_S3_TIMEOUT_MS
        http_client_core = components["http-client-core"]
        if http_client_core is None:
            raise RuntimeError("http-client-core component config must be present")
        http_client_core["testsuite-timeout"] = "2s"

    return _patch


@pytest.fixture(scope="session")
def service_config_path_temp(
    service_tmpdir,
    _service_config_hooked,
    service_binary: pathlib.Path,
    service_source_dir: pathlib.Path,
    testsuite_webshotd_state_dir: pathlib.Path,
    s3_bucket_name: str,
) -> pathlib.Path:
    dst_path = service_tmpdir / "config.yaml"
    config_yaml = dict(_service_config_hooked.config_yaml)
    config_vars = dict(_service_config_hooked.config_vars)
    runtime_layout = runtime_layout_from_binary(service_binary)
    config_vars["rapidoc_assets_dir"] = str(runtime_layout.rapidoc_assets_dir)
    config_vars["openapi_public_dir"] = str(service_source_dir.parent / "schema" / "public")
    config_vars["openapi_admin_dir"] = str(service_source_dir.parent / "schema" / "admin")
    config_vars["openapi_common_dir"] = str(service_source_dir.parent / "schema" / "common")
    config_vars["web_ui_dir"] = str(runtime_layout.web_ui_dir)
    config_vars["web_ui_vendor_dir"] = str(runtime_layout.web_ui_vendor_dir)
    config_vars["state_dir"] = str(testsuite_webshotd_state_dir)
    config_vars["s3_bucket"] = s3_bucket_name
    config_vars["public_base_url"] = f"http://127.0.0.1:8333/{s3_bucket_name}"
    config_vars.update(task_processor_config_vars(config_yaml))

    components = config_yaml["components_manager"]["components"]
    dynamic_config_defaults = components["dynamic-config"]["defaults"]
    dynamic_config_defaults["POSTGRES_CONNECTION_POOL_SETTINGS"] = {
        "__default__": {
            "min_pool_size": 1,
            "max_pool_size": 2,
            "max_queue_size": 200,
            "connecting_limit": 1,
        }
    }
    browser_probe = components["browser_probe"]
    browser_probe["testsuite_loopback_ports"] = [_require_service_listener_port(config_yaml)]

    if not config_vars:
        config_yaml.pop("config_vars", None)
    else:
        config_vars_path = service_tmpdir / "config_vars.yaml"
        config_vars_path.write_text(yaml.dump(config_vars))
        config_yaml["config_vars"] = str(config_vars_path)

    dst_path.write_text(yaml.dump(config_yaml))
    return dst_path


@pytest.fixture
def extra_client_deps(pg_gate_ready, s3_bucket_ready):
    return [pg_gate_ready]


@pytest.fixture
def browser_probe(monitor_client):
    async def _probe(
        url: str,
        *,
        wait_expression: str,
        frame_expression: str | None = None,
        timeout_ms: int = 15_000,
    ) -> dict:
        payload = {
            "url": url,
            "wait_expression": wait_expression,
            "timeout_ms": timeout_ms,
        }
        if frame_expression is not None:
            payload["frame_expression"] = frame_expression
        resp = await monitor_client.post(
            "/tests/browser-probe",
            json=payload,
        )
        assert resp.status == 200, resp.text
        return resp.json()

    return _probe


USERVER_CONFIG_HOOKS = ["patch_s3_config"]
