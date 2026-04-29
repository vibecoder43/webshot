from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import cast

import yaml

from s6.common import ToolError, die, repo_root_from_file

WEBSHOTD_READY_URL = "http://127.0.0.1:8081/service/monitor?format=json"
RUNTIME_MODES = ("dev", "prodlike", "prod")


@dataclass(frozen=True)
class ServiceSpec:
    name: str
    service_dir: Path
    log_file: Path
    ready_cmd: list[str | Path]
    timeout_sec: float


@dataclass(frozen=True)
class RuntimeStateContext:
    mode: str
    repo_root: Path
    state_dir: Path
    scan_dir: Path
    svscan_pid_file: Path

    def supervised_service_dir(self, name: str) -> Path:
        return self.scan_dir / name

    def runtime_log_file(self, name: str) -> Path:
        return self.state_dir / name

    @property
    def managed_cgroup_root_file(self) -> Path:
        return self.state_dir / "managed_cgroup_root"

    @property
    def runtime_config_vars_path(self) -> Path:
        return self.state_dir / "config_vars.yaml"


@dataclass(frozen=True)
class RuntimeInspectContext(RuntimeStateContext):
    service_profile: str

    def load_config_vars(self) -> dict[str, object]:
        return read_yaml(self.runtime_config_vars_path)


@dataclass(frozen=True)
class RuntimeUpContext(RuntimeInspectContext):
    binary_path: Path
    config_vars_source: Path
    runtime_ld_library_path: str | None
    seaweedfs_s3_config_path: Path | None

    def load_config_vars(self) -> dict[str, object]:
        if self.runtime_config_vars_path.is_file():
            return read_yaml(self.runtime_config_vars_path)
        return read_yaml(self.config_vars_source)

    @property
    def postgres_dir(self) -> Path:
        return self.state_dir / "postgres"

    @property
    def postgres_data_dir(self) -> Path:
        return self.postgres_dir / "data"

    @property
    def postgres_run_dir(self) -> Path:
        return self.postgres_dir / "run"

    @property
    def postgres_bootstrap_done_file(self) -> Path:
        return self.postgres_dir / ".bootstrap_complete"

    @property
    def postgres_bootstrap_log(self) -> Path:
        return self.state_dir / "postgres_bootstrap.log"

    @property
    def seaweed_data_dir(self) -> Path:
        return self.state_dir / "seaweed"

    @property
    def test_target_dir(self) -> Path:
        return self.state_dir / "test-target"

    @property
    def webshotd_state_dir(self) -> Path:
        return self.state_dir / "webshotd"

    @property
    def webshotd_config_vars_override_path(self) -> Path:
        return self.state_dir / "webshotd_config_vars_override.yaml"

    @property
    def test_pki_dir(self) -> Path:
        return self.webshotd_state_dir / "test_pki"

    @property
    def trusted_ca_cert_path(self) -> Path:
        return self.test_pki_dir / "trusted_root_ca.crt"

    @property
    def trusted_ca_key_path(self) -> Path:
        return self.test_pki_dir / "trusted_root_ca.key"

    @property
    def trusted_cert_path(self) -> Path:
        return self.test_pki_dir / "trusted_test_target.crt"

    @property
    def trusted_key_path(self) -> Path:
        return self.test_pki_dir / "trusted_test_target.key"

    @property
    def untrusted_cert_path(self) -> Path:
        return self.test_pki_dir / "untrusted_test_target.crt"

    @property
    def untrusted_key_path(self) -> Path:
        return self.test_pki_dir / "untrusted_test_target.key"

    @property
    def trusted_nssdb_dir(self) -> Path:
        return self.test_pki_dir / "chromium_nssdb"

    @property
    def nginx_payload_dir(self) -> Path:
        return self.state_dir / "nginx_payloads"

    @property
    def generated_nginx_config_path(self) -> Path:
        return self.test_target_dir / "nginx.conf"


def read_yaml(path: Path) -> dict[str, object]:
    try:
        raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    except FileNotFoundError as e:
        raise ToolError(message=f"Missing config vars file: {path}", exit_code=2) from e

    if not isinstance(raw, dict):
        die(f"Config vars file must be a YAML mapping: {path}", exit_code=2)
    return raw


def require_cmake_cache_string(path: Path, key: str) -> str:
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


def require_yaml_string(raw: dict[str, object], key: str, *, source: Path) -> str:
    value = raw.get(key)
    if not isinstance(value, str) or not value:
        die(f"Missing required config var '{key}' in {source}", exit_code=2)
    return cast(str, value)


def build_state_context(
    *,
    mode: str,
    layout_root: str | None = None,
    state_dir: str | None = None,
) -> RuntimeStateContext:
    resolved_repo_root = (
        Path(layout_root).resolve() if layout_root else repo_root_from_file(Path(__file__))
    )
    resolved_state_dir = Path(state_dir).resolve() if state_dir else Path("/tmp/webshot") / mode
    return RuntimeStateContext(
        mode=mode,
        repo_root=resolved_repo_root,
        state_dir=resolved_state_dir,
        scan_dir=resolved_state_dir / "s6_scan",
        svscan_pid_file=resolved_state_dir / "s6_svscan.pid",
    )


def build_inspect_context(
    *,
    mode: str,
    service_profile: str,
    layout_root: str | None = None,
    state_dir: str | None = None,
) -> RuntimeInspectContext:
    if service_profile == "test_infra" and mode != "dev":
        die("service profile 'test_infra' requires --mode dev", exit_code=2)

    state_ctx = build_state_context(mode=mode, layout_root=layout_root, state_dir=state_dir)
    return RuntimeInspectContext(
        mode=state_ctx.mode,
        repo_root=state_ctx.repo_root,
        state_dir=state_ctx.state_dir,
        scan_dir=state_ctx.scan_dir,
        svscan_pid_file=state_ctx.svscan_pid_file,
        service_profile=service_profile,
    )


def build_up_context(
    *,
    mode: str,
    service_profile: str,
    layout_root: str | None,
    state_dir: str | None,
    binary_path: str,
    config_vars_source: str,
    runtime_ld_library_path: str | None,
    seaweedfs_s3_config_path: str | None,
) -> RuntimeUpContext:
    inspect_ctx = build_inspect_context(
        mode=mode,
        service_profile=service_profile,
        layout_root=layout_root,
        state_dir=state_dir,
    )
    return RuntimeUpContext(
        mode=inspect_ctx.mode,
        repo_root=inspect_ctx.repo_root,
        state_dir=inspect_ctx.state_dir,
        scan_dir=inspect_ctx.scan_dir,
        svscan_pid_file=inspect_ctx.svscan_pid_file,
        service_profile=service_profile,
        binary_path=Path(binary_path),
        config_vars_source=Path(config_vars_source),
        runtime_ld_library_path=runtime_ld_library_path,
        seaweedfs_s3_config_path=(
            Path(seaweedfs_s3_config_path) if seaweedfs_s3_config_path else None
        ),
    )
