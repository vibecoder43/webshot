from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from s6.common import die
from s6.runtime_context import require_yaml_string

_RUNTIME_MODE_VALUES = ("local", "external")
_LOCAL_POSTGRES_HOSTS = {"", "localhost", "127.0.0.1", "::1"}


@dataclass(frozen=True)
class RuntimeDependencyModes:
    pg_mode: str
    s3_mode: str


@dataclass(frozen=True)
class LocalS3BootstrapConfig:
    bucket: str
    endpoint: str
    secure: bool
    secrets_path: Path


def require_yaml_choice(
    raw_vars: dict[str, object],
    key: str,
    *,
    source: Path,
    allowed: tuple[str, ...],
) -> str:
    value = require_yaml_string(raw_vars, key, source=source)
    if value not in allowed:
        choices = ", ".join(allowed)
        die(f"Config var '{key}' in {source} must be one of: {choices}", exit_code=2)
    return value


def resolve_runtime_dependency_modes(
    raw_vars: dict[str, object],
    *,
    source: Path,
) -> RuntimeDependencyModes:
    return RuntimeDependencyModes(
        pg_mode=require_yaml_choice(
            raw_vars, "pg_mode", source=source, allowed=_RUNTIME_MODE_VALUES
        ),
        s3_mode=require_yaml_choice(
            raw_vars, "s3_mode", source=source, allowed=_RUNTIME_MODE_VALUES
        ),
    )


def postgres_dsn_ownership_mode(*, dsn: str) -> str:
    parsed = urlsplit(dsn)
    host = parsed.hostname
    if host is not None:
        return "local" if host in _LOCAL_POSTGRES_HOSTS else "external"

    query_host = parse_qs(parsed.query).get("host", [])
    if not query_host:
        return "local"

    host_value = query_host[0]
    if host_value.startswith("/"):
        return "local"
    return "local" if host_value in _LOCAL_POSTGRES_HOSTS else "external"


def resolve_local_s3_bootstrap_config(
    *,
    repo_root: Path,
    raw_vars: dict[str, object],
    source: Path,
) -> LocalS3BootstrapConfig:
    bucket = require_yaml_string(raw_vars, "s3_bucket", source=source)
    endpoint = require_yaml_string(raw_vars, "s3_endpoint", source=source)
    secdist_path = Path(require_yaml_string(raw_vars, "secdist_path", source=source))

    parsed = urlsplit(endpoint)
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        die(
            f"Config var 's3_endpoint' in {source} must be "
            "http(s)://host[:port] for local S3 bootstrap",
            exit_code=2,
        )
    if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        die(
            f"Config var 's3_endpoint' in {source} must not include a path, query, or fragment",
            exit_code=2,
        )

    if not secdist_path.is_absolute():
        secdist_path = repo_root / "webshotd" / secdist_path

    return LocalS3BootstrapConfig(
        bucket=bucket,
        endpoint=parsed.netloc,
        secure=parsed.scheme == "https",
        secrets_path=secdist_path,
    )
