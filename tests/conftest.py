import pathlib

import psycopg2
import psycopg2.extras
import pytest
from testsuite.databases.pgsql import discover

pytest_plugins = [
    "pytest_userver.plugins.core",
    "pytest_userver.plugins.postgresql",
    "pytest_userver.plugins.config",
    "helpers.sql_loader",
]

psycopg2.extras.register_uuid()


@pytest.fixture(scope="session")
def pgsql_local(pgsql_local_create, service_source_dir: pathlib.Path):
    schemas = discover.find_schemas(
        None,
        [service_source_dir / "sql" / "schema"],
    )
    return pgsql_local_create(list(schemas.values()))


@pytest.fixture(scope="session")
def userver_pg_config(pgsql_local):
    db_uri = {name: conn.get_uri() for name, conn in pgsql_local.items()}

    def _patch(config_yaml, _config_vars):
        components = config_yaml["components_manager"]["components"]
        overrides = {
            "capture-meta-db": "capture_meta_db_schema",
            "shared-state-db": "shared_state_db_schema",
        }
        for component_name, dbname in overrides.items():
            conninfo = db_uri[dbname]
            component = components[component_name]
            component["dbconnection"] = conninfo
            component.pop("dbalias", None)

    return _patch


@pytest.fixture(scope="session")
def service_secdist_path(service_source_dir: pathlib.Path) -> pathlib.Path:
    return service_source_dir / "secrets" / "test_secdist.json"


@pytest.fixture(scope="session")
def service_env(service_source_dir: pathlib.Path):
    return {
        "LSAN_OPTIONS": f"suppressions={service_source_dir}/lsan.supp",
    }


@pytest.fixture(scope="session")
def allowed_url_prefixes_extra():
    # Permit S3 uploads to the local SeaweedFS endpoint during tests.
    return ["http://localhost:8333/"]
