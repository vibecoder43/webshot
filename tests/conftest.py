import asyncio
import pathlib
from urllib.parse import urlparse, urlunparse

import psycopg2
import psycopg2.extras
import pytest
from helpers.s3_bucket import ensure_s3_bucket_exists
from pytest_userver import chaos
from testsuite.databases.pgsql import discover

pytest_plugins = [
    "pytest_userver.plugins.core",
    "pytest_userver.plugins.postgresql",
    "pytest_userver.plugins.config",
    "pytest_userver.chaos",
    "helpers.sql_loader",
]

psycopg2.extras.register_uuid()


@pytest.fixture(scope="session")
def service_port() -> int:
    return 8080


@pytest.fixture(scope="session")
def monitor_port() -> int:
    return 8081


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
async def s3_gate():
    # Proxy between service and local S3 (SeaweedFS): service -> gate :8334 -> S3 :8333.
    route = chaos.GateRoute(
        name="s3",
        host_to_server="localhost",
        port_to_server=8333,
        host_for_client="localhost",
        port_for_client=8334,
    )
    loop = asyncio.get_event_loop()
    gate = chaos.TcpGate(route, loop)
    gate.start()
    try:
        yield gate
    finally:
        await gate.stop()


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


@pytest.fixture
def extra_client_deps(pg_gate_ready, s3_gate_ready):
    # Ensure gates are reset and running before service_client is created.
    return [pg_gate_ready, s3_gate_ready]


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
def allowed_url_prefixes_extra():
    # Permit S3 uploads to the chaos gate in front of the local SeaweedFS endpoint.
    return ["http://localhost:8334/"]


def patch_s3_config(config_yaml, _config_vars):
    components = config_yaml["components_manager"]["components"]
    webshot_cfg = components["webshot_config"]
    webshot_cfg["s3_endpoint"] = "http://localhost:8334"


USERVER_CONFIG_HOOKS = [patch_s3_config]
