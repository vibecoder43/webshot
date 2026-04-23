from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

from s6.common import die
from s6.runtime_config import postgres_dsn_ownership_mode, resolve_runtime_dependency_modes
from s6.runtime_context import require_yaml_string


@dataclass(frozen=True)
class RuntimeDatabaseDefinition:
    name: str
    dsn_config_key: str
    schema_dir_name: str

    def base_dir(self, repo_root: Path) -> Path:
        return repo_root / "webshotd" / "sql" / "schema" / self.schema_dir_name


@dataclass(frozen=True)
class ResolvedRuntimeDatabase:
    definition: RuntimeDatabaseDefinition
    dsn: str
    db_name: str
    base_dir: Path


def all_runtime_databases() -> tuple[RuntimeDatabaseDefinition, ...]:
    return (
        RuntimeDatabaseDefinition(
            name="capture_meta_db",
            dsn_config_key="pg_capture_meta_db_dsn",
            schema_dir_name="capture_meta_db",
        ),
        RuntimeDatabaseDefinition(
            name="shared_state_db",
            dsn_config_key="pg_shared_state_db_dsn",
            schema_dir_name="shared_state_db",
        ),
    )


def resolve_runtime_databases(
    *,
    repo_root: Path,
    raw_vars: dict[str, object],
    source: Path,
) -> list[ResolvedRuntimeDatabase]:
    dependency_modes = resolve_runtime_dependency_modes(raw_vars, source=source)
    resolved: list[ResolvedRuntimeDatabase] = []
    ownership_modes: dict[str, str] = {}
    for definition in all_runtime_databases():
        dsn = require_yaml_string(raw_vars, definition.dsn_config_key, source=source)
        ownership_modes[definition.name] = postgres_dsn_ownership_mode(dsn=dsn)
        resolved.append(
            ResolvedRuntimeDatabase(
                definition=definition,
                dsn=dsn,
                db_name=_database_name_from_dsn(
                    dsn,
                    key=definition.dsn_config_key,
                    source=source,
                ),
                base_dir=definition.base_dir(repo_root),
            )
        )
    _validate_postgres_ownership(
        ownership_modes=ownership_modes,
        expected_mode=dependency_modes.pg_mode,
        source=source,
    )
    return resolved


def _database_name_from_dsn(dsn: str, *, key: str, source: Path) -> str:
    parsed = urlsplit(dsn)
    db_name = parsed.path.lstrip("/")
    if not db_name:
        die(f"Config var '{key}' in {source} must include a database name", exit_code=2)
    return db_name


def _validate_postgres_ownership(
    *,
    ownership_modes: dict[str, str],
    expected_mode: str,
    source: Path,
) -> None:
    unique_modes = set(ownership_modes.values())
    if len(unique_modes) != 1:
        detail = ", ".join(f"{name}={mode}" for name, mode in sorted(ownership_modes.items()))
        die(
            (
                "Mixed Postgres ownership between configured databases is unsupported "
                f"in {source}: {detail}"
            ),
            exit_code=2,
        )

    actual_mode = next(iter(unique_modes))
    if actual_mode != expected_mode:
        die(
            (
                f"Config var 'pg_mode' in {source} is '{expected_mode}', "
                f"but the configured database DSNs point to '{actual_mode}' postgres ownership"
            ),
            exit_code=2,
        )
