from __future__ import annotations

import hashlib
import shlex
import shutil
from dataclasses import dataclass
from pathlib import Path

from s6.common import ToolError, die, need_cmd, run
from s6.runtime_config import (
    resolve_local_s3_bootstrap_config,
    resolve_runtime_dependency_modes,
)
from s6.runtime_context import (
    RuntimeStateContext,
    RuntimeUpContext,
    ServiceSpec,
    read_yaml,
)
from s6.runtime_databases import ResolvedRuntimeDatabase, resolve_runtime_databases
from s6.runtime_services import (
    active_service_definitions,
    prepare_service_runtime,
    render_service_tree,
)
from s6.s3_bucket import ensure_s3_bucket_exists


@dataclass(frozen=True)
class _BootstrappedDatabase:
    resolved: ResolvedRuntimeDatabase
    migrations_hash: str


def prepare_runtime(ctx: RuntimeUpContext) -> list[ServiceSpec]:
    snapshot_runtime_config_vars(ctx)
    _validate_seaweedfs_s3_config_arg(ctx)
    definitions = active_service_definitions(ctx)
    ctx.state_dir.mkdir(parents=True, exist_ok=True)
    _bootstrap_postgres(ctx)
    if ctx.scan_dir.exists():
        shutil.rmtree(ctx.scan_dir)
    prepare_service_runtime(ctx, definitions)
    return render_service_tree(ctx, definitions)


def _validate_seaweedfs_s3_config_arg(ctx: RuntimeUpContext) -> None:
    raw_vars = read_yaml(ctx.runtime_config_vars_path)
    dependency_modes = resolve_runtime_dependency_modes(
        raw_vars, source=ctx.runtime_config_vars_path
    )
    if dependency_modes.s3_mode != "local":
        if ctx.seaweedfs_s3_config_path is not None:
            die("--seaweedfs-s3-config requires s3_mode: local", exit_code=2)
        return

    if ctx.seaweedfs_s3_config_path is not None and not ctx.seaweedfs_s3_config_path.is_file():
        die(
            f"Missing SeaweedFS S3 config file: {ctx.seaweedfs_s3_config_path}",
            exit_code=2,
        )

    if ctx.service_profile == "test_infra":
        return
    if ctx.seaweedfs_s3_config_path is None:
        die("--seaweedfs-s3-config is required when non-test s3_mode is local", exit_code=2)


def ensure_local_s3_bootstrap(ctx: RuntimeStateContext) -> None:
    raw_vars = read_yaml(ctx.runtime_config_vars_path)
    dependency_modes = resolve_runtime_dependency_modes(
        raw_vars, source=ctx.runtime_config_vars_path
    )
    if dependency_modes.s3_mode != "local":
        return

    bootstrap = resolve_local_s3_bootstrap_config(
        repo_root=ctx.repo_root,
        raw_vars=raw_vars,
        source=ctx.runtime_config_vars_path,
    )
    ensure_s3_bucket_exists(
        secrets_path=bootstrap.secrets_path,
        s3_url=bootstrap.endpoint,
        bucket=bootstrap.bucket,
        secure=bootstrap.secure,
    )


def _bootstrap_postgres(ctx: RuntimeUpContext) -> None:
    need_cmd("pgmigrate")

    raw_vars = read_yaml(ctx.runtime_config_vars_path)
    dependency_modes = resolve_runtime_dependency_modes(
        raw_vars, source=ctx.runtime_config_vars_path
    )
    databases = _bootstrap_databases(ctx, raw_vars)

    bootstrap_state = "".join(
        [
            *(
                f"{database.resolved.definition.name}={database.resolved.db_name}\n"
                for database in databases
            ),
            *(
                f"{database.resolved.definition.name}_migrations={database.migrations_hash}\n"
                for database in databases
            ),
            f"pg_mode={dependency_modes.pg_mode}\n",
        ]
    )
    if (
        ctx.postgres_bootstrap_done_file.is_file()
        and ctx.postgres_bootstrap_done_file.read_text(encoding="utf-8") == bootstrap_state
    ):
        return

    if dependency_modes.pg_mode == "external":
        for database in databases:
            _die_if_legacy_db_needs_baseline(
                dsn=database.resolved.dsn,
                base_dir=database.resolved.base_dir,
                db_name=database.resolved.db_name,
            )
            _pgmigrate_migrate(
                dsn=database.resolved.dsn,
                base_dir=database.resolved.base_dir,
            )
        ctx.postgres_dir.mkdir(parents=True, exist_ok=True)
        ctx.postgres_bootstrap_done_file.write_text(bootstrap_state, encoding="utf-8")
        return

    need_cmd("initdb")
    need_cmd("pg_ctl")
    need_cmd("psql")
    need_cmd("createdb")

    was_initialized = ctx.postgres_data_dir.joinpath("PG_VERSION").is_file()
    ctx.postgres_dir.mkdir(parents=True, exist_ok=True)
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
        for database in databases:
            _ensure_postgres_database(database.resolved.db_name)
            _die_if_legacy_db_needs_baseline(
                dsn=database.resolved.dsn,
                base_dir=database.resolved.base_dir,
                db_name=database.resolved.db_name,
            )
            _pgmigrate_migrate(
                dsn=database.resolved.dsn,
                base_dir=database.resolved.base_dir,
            )

        ctx.postgres_bootstrap_done_file.write_text(bootstrap_state, encoding="utf-8")
    finally:
        run(
            ["pg_ctl", "-D", str(ctx.postgres_data_dir), "-m", "fast", "-w", "stop"],
            timeout_sec=120.0,
        )


def _bootstrap_databases(
    ctx: RuntimeUpContext,
    raw_vars: dict[str, object],
) -> list[_BootstrappedDatabase]:
    return [
        _BootstrappedDatabase(
            resolved=database,
            migrations_hash=_hash_pgmigrate_base_dir(database.base_dir),
        )
        for database in resolve_runtime_databases(
            repo_root=ctx.repo_root,
            raw_vars=raw_vars,
            source=ctx.runtime_config_vars_path,
        )
    ]


def snapshot_runtime_config_vars(ctx: RuntimeUpContext) -> None:
    ctx.state_dir.mkdir(parents=True, exist_ok=True)
    try:
        raw = ctx.config_vars_source.read_text(encoding="utf-8")
    except FileNotFoundError as e:
        raise ToolError(
            message=f"Missing config vars file: {ctx.config_vars_source}", exit_code=2
        ) from e
    ctx.runtime_config_vars_path.write_text(raw, encoding="utf-8")


def _ensure_postgres_database(db_name: str) -> None:
    exists = run(
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
            f"SELECT 1 FROM pg_database WHERE datname = '{db_name}'",
        ],
        capture=True,
        timeout_sec=30.0,
    ).stdout.strip()
    if exists == "1":
        return
    run(
        ["createdb", "-h", "127.0.0.1", "-p", "5432", "-U", "postgres", db_name],
        timeout_sec=30.0,
    )


def _psql_scalar(*, db: str, query: str) -> str:
    need_cmd("psql")
    return run(
        [
            "psql",
            "-h",
            "127.0.0.1",
            "-p",
            "5432",
            "-U",
            "postgres",
            "-d",
            db,
            "-tAc",
            query,
        ],
        capture=True,
        timeout_sec=30.0,
    ).stdout.strip()


def _pgmigrate_migrate(*, dsn: str, base_dir: Path) -> None:
    need_cmd("pgmigrate")
    run(
        [
            "pgmigrate",
            "-c",
            dsn,
            "-d",
            str(base_dir),
            "-t",
            "latest",
            "-vv",
            "migrate",
        ],
        timeout_sec=120.0,
    )


def _die_if_legacy_db_needs_baseline(*, dsn: str, base_dir: Path, db_name: str) -> None:
    schema_version_exists = _psql_scalar(
        db=db_name,
        query="select to_regclass('public.schema_version') is not null",
    )
    if schema_version_exists == "t":
        return

    has_tables = _psql_scalar(
        db=db_name,
        query=(
            "select exists ("
            "  select 1"
            "  from information_schema.tables"
            "  where table_schema = 'public'"
            "    and table_type = 'BASE TABLE'"
            "    and table_name <> 'schema_version'"
            ")"
        ),
    )
    if has_tables != "t":
        return

    die(
        "\n".join(
            [
                f"Detected legacy database without pgmigrate schema_version: {db_name}",
                "This repo no longer bootstraps schema from snapshot SQL files.",
                "Baseline manually (only if the schema matches V0001), then restart:",
                f"  pgmigrate -c {shlex.quote(dsn)} -d {shlex.quote(str(base_dir))} -b 1 baseline",
            ]
        ),
        exit_code=2,
    )


def _hash_pgmigrate_base_dir(base_dir: Path) -> str:
    migrations_yaml = base_dir / "migrations.yml"
    migrations_dir = base_dir / "migrations"
    if not migrations_yaml.is_file():
        die(f"Missing pgmigrate config: {migrations_yaml}", exit_code=2)
    if not migrations_dir.is_dir():
        die(f"Missing pgmigrate migrations dir: {migrations_dir}", exit_code=2)

    files = [migrations_yaml, *[path for path in migrations_dir.rglob("*") if path.is_file()]]
    return _hash_file_inputs(files)


def _hash_file_inputs(files: list[Path]) -> str:
    hasher = hashlib.sha256()
    for path in sorted(files, key=lambda candidate: str(candidate)):
        hasher.update(str(path).encode("utf-8"))
        hasher.update(b"\0")
        hasher.update(path.read_bytes())
        hasher.update(b"\0")
    return hasher.hexdigest()
