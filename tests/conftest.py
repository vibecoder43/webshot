import pathlib

import psycopg2.extras
import pytest

from testsuite.databases import pgsql as testsuite_pgsql

pytest_plugins = [
    "testsuite.pytest_plugin",
    "pytest_userver.plugins.postgresql",
    "helpers.sql_loader",
]

psycopg2.extras.register_uuid()


@pytest.fixture(scope="session")
def pgsql_local(service_source_dir: pathlib.Path, pgsql_local_create):
    schema_dir = service_source_dir / "sql" / "schema"
    databases = testsuite_pgsql.discover.find_schemas("webshot", [schema_dir])
    return pgsql_local_create(list(databases.values()))


@pytest.fixture(scope="session")
def userver_pg_config(pgsql_local):
    meta_uri = pgsql_local["webshot_meta_db_schema"].get_uri()
    deny_uri = pgsql_local["denylist_db_schema"].get_uri()

    def _patch(config_yaml, config_vars):
        components = config_yaml["components_manager"]["components"]

        meta_cfg = components.get("webshot-meta-db")
        if meta_cfg and "dbconnection" in meta_cfg:
            meta_cfg["dbconnection"] = meta_uri

        deny_cfg = components.get("denylist-db")
        if deny_cfg and "dbconnection" in deny_cfg:
            deny_cfg["dbconnection"] = deny_uri

    return _patch
