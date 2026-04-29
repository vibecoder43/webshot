from __future__ import annotations

import shutil
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import yaml

from s6.common import need_cmd, run
from s6.runtime_config import resolve_runtime_dependency_modes
from s6.runtime_context import (
    WEBSHOTD_READY_URL,
    RuntimeInspectContext,
    RuntimeUpContext,
    ServiceSpec,
    require_cmake_cache_string,
)
from s6.runtime_support import (
    finish_script,
    shell_join,
    shell_quote,
    wait_script,
    write_executable,
    write_text,
)
from s6.userver_task_processors import fs_worker_threads

TEST_HOST = "test-target"
TEST_ASSET_HOST = "asset.test-target"
UNTRUSTED_TEST_HOST = "untrusted.test-target"
TRUSTED_CA_COMMON_NAME = "webshot_test_target_origin_ca"

_ServicePredicate = Callable[[RuntimeInspectContext], bool]
_PrepareHook = Callable[[RuntimeUpContext], None]
_ReadyCmdBuilder = Callable[[RuntimeInspectContext], list[str | Path]]
_ScriptsBuilder = Callable[[RuntimeUpContext], "ServiceScripts"]


@dataclass(frozen=True)
class ServiceScripts:
    check_cmd: list[str | Path]
    run_cmd: list[str | Path]
    cwd: Path
    preamble: str = ""


@dataclass(frozen=True)
class RuntimeServiceDefinition:
    name: str
    service_dir_name: str
    log_file_name: str
    timeout_sec: float
    enabled: _ServicePredicate
    ready_cmd: _ReadyCmdBuilder
    scripts: _ScriptsBuilder
    prepare: _PrepareHook | None = None

    def service_dir(self, ctx: RuntimeInspectContext) -> Path:
        return ctx.supervised_service_dir(self.service_dir_name)

    def log_file(self, ctx: RuntimeInspectContext) -> Path:
        return ctx.runtime_log_file(self.log_file_name)

    def resolve(self, ctx: RuntimeInspectContext) -> ServiceSpec:
        return ServiceSpec(
            name=self.name,
            service_dir=self.service_dir(ctx),
            log_file=self.log_file(ctx),
            ready_cmd=self.ready_cmd(ctx),
            timeout_sec=self.timeout_sec,
        )


def all_service_definitions() -> tuple[RuntimeServiceDefinition, ...]:
    return (
        RuntimeServiceDefinition(
            name="postgres",
            service_dir_name="postgres",
            log_file_name="postgres.log",
            timeout_sec=30.0,
            enabled=_postgres_local_only,
            ready_cmd=_postgres_ready_cmd,
            scripts=_postgres_scripts,
        ),
        RuntimeServiceDefinition(
            name="seaweedfs",
            service_dir_name="seaweedfs",
            log_file_name="seaweedfs.log",
            timeout_sec=60.0,
            enabled=_local_s3_only,
            ready_cmd=_seaweedfs_ready_cmd,
            scripts=_seaweedfs_scripts,
            prepare=_prepare_seaweedfs,
        ),
        RuntimeServiceDefinition(
            name="test_target",
            service_dir_name="test_target",
            log_file_name="test-target.log",
            timeout_sec=30.0,
            enabled=_dev_only,
            ready_cmd=_test_target_ready_cmd,
            scripts=_test_target_scripts,
            prepare=_prepare_test_target,
        ),
        RuntimeServiceDefinition(
            name="webshotd",
            service_dir_name="webshotd",
            log_file_name="webshotd.log",
            timeout_sec=30.0,
            enabled=_full_profile_only,
            ready_cmd=_webshotd_ready_cmd,
            scripts=_webshotd_scripts,
        ),
    )


def active_service_definitions(ctx: RuntimeInspectContext) -> list[RuntimeServiceDefinition]:
    return [definition for definition in all_service_definitions() if definition.enabled(ctx)]


def active_service_specs(ctx: RuntimeInspectContext) -> list[ServiceSpec]:
    return [definition.resolve(ctx) for definition in active_service_definitions(ctx)]


def known_service_dirs(ctx: RuntimeInspectContext) -> dict[str, Path]:
    return {
        definition.name: definition.service_dir(ctx) for definition in all_service_definitions()
    }


def prepare_service_runtime(
    ctx: RuntimeUpContext,
    definitions: list[RuntimeServiceDefinition],
) -> None:
    for definition in definitions:
        if definition.prepare is not None:
            definition.prepare(ctx)


def render_service_tree(
    ctx: RuntimeUpContext,
    definitions: list[RuntimeServiceDefinition],
) -> list[ServiceSpec]:
    specs: list[ServiceSpec] = []
    for definition in definitions:
        spec = definition.resolve(ctx)
        _write_service(spec, definition.scripts(ctx))
        specs.append(spec)
    return specs


def _always_enabled(ctx: RuntimeInspectContext) -> bool:
    del ctx
    return True


def _dependency_modes(ctx: RuntimeInspectContext) -> tuple[str, str]:
    modes = resolve_runtime_dependency_modes(
        ctx.load_config_vars(), source=ctx.runtime_config_vars_path
    )
    return modes.pg_mode, modes.s3_mode


def _postgres_local_only(ctx: RuntimeInspectContext) -> bool:
    pg_mode, _ = _dependency_modes(ctx)
    return pg_mode == "local"


def _local_s3_only(ctx: RuntimeInspectContext) -> bool:
    _, s3_mode = _dependency_modes(ctx)
    return s3_mode == "local"


def _dev_only(ctx: RuntimeInspectContext) -> bool:
    return ctx.mode == "dev"


def _full_profile_only(ctx: RuntimeInspectContext) -> bool:
    return ctx.service_profile == "full"


def _postgres_ready_cmd(ctx: RuntimeInspectContext) -> list[str | Path]:
    del ctx
    return ["pg_isready", "-h", "127.0.0.1", "-p", "5432", "-U", "postgres"]


def _postgres_scripts(ctx: RuntimeUpContext) -> ServiceScripts:
    return ServiceScripts(
        check_cmd=_postgres_ready_cmd(ctx),
        run_cmd=[
            "postgres",
            "-D",
            str(ctx.postgres_data_dir),
            "-h",
            "127.0.0.1",
            "-k",
            str(ctx.postgres_run_dir),
            "-p",
            "5432",
            "-c",
            "timezone=UTC",
            "-c",
            "log_timezone=UTC",
        ],
        cwd=ctx.repo_root,
    )


def _seaweedfs_ready_cmd(ctx: RuntimeInspectContext) -> list[str | Path]:
    del ctx
    return [sys.executable, "-m", "s6.check_seaweedfs_ready"]


def _prepare_seaweedfs(ctx: RuntimeUpContext) -> None:
    ctx.seaweed_data_dir.mkdir(parents=True, exist_ok=True)


def _seaweedfs_scripts(ctx: RuntimeUpContext) -> ServiceScripts:
    return ServiceScripts(
        check_cmd=_seaweedfs_ready_cmd(ctx),
        run_cmd=[
            "weed",
            "server",
            "-s3",
            "-filer",
            "-ip.bind=0.0.0.0",
            f"-dir={ctx.seaweed_data_dir}",
            "-filer.concurrentUploadLimitMB=512",
            "-filer.maxMB=16",
            "-volume.concurrentUploadLimitMB=512",
            "-volume.max=256",
            "-volume.port=8082",
            "-volume.port.grpc=18082",
            "-master.volumeSizeLimitMB=256",
            "-metricsPort=9324",
            f"-s3.config={ctx.repo_root / 'seaweedfs/s3_config.json'}",
        ],
        cwd=ctx.repo_root,
    )


def _test_target_ready_cmd(ctx: RuntimeInspectContext) -> list[str | Path]:
    del ctx
    return [sys.executable, "-m", "s6.check_http_ready", "http://127.0.0.1:18080/"]


def _prepare_test_target(ctx: RuntimeUpContext) -> None:
    ctx.test_target_dir.mkdir(parents=True, exist_ok=True)
    _generate_local_fixture_pki(ctx)
    _render_test_target_nginx_config(ctx)


def _test_target_scripts(ctx: RuntimeUpContext) -> ServiceScripts:
    return ServiceScripts(
        check_cmd=_test_target_ready_cmd(ctx),
        run_cmd=[
            "nginx",
            "-e",
            "stderr",
            "-p",
            ctx.repo_root,
            "-c",
            ctx.generated_nginx_config_path,
            "-g",
            f"daemon off; pid {ctx.test_target_dir / 'nginx.pid'};",
        ],
        cwd=ctx.repo_root,
    )


def _webshotd_ready_cmd(ctx: RuntimeInspectContext) -> list[str | Path]:
    del ctx
    return [sys.executable, "-m", "s6.check_webshotd_ready", WEBSHOTD_READY_URL]


def _webshotd_scripts(ctx: RuntimeUpContext) -> ServiceScripts:
    rapidoc_assets_dir = require_cmake_cache_string(
        ctx.binary_path.parent / "CMakeCache.txt",
        "WEBSHOT_RAPIDOC_ASSETS_DIR",
    )
    static_config_path = ctx.repo_root / "webshotd/config/static_config.yaml"
    static_config = yaml.safe_load(static_config_path.read_text(encoding="utf-8"))
    write_text(
        ctx.webshotd_config_vars_override_path,
        yaml.safe_dump(
            {
                "fs_worker_threads": fs_worker_threads(static_config),
                "rapidoc_assets_dir": rapidoc_assets_dir,
                "openapi_public_dir": str(ctx.repo_root / "schema" / "public"),
                "openapi_admin_dir": str(ctx.repo_root / "schema" / "admin"),
                "openapi_common_dir": str(ctx.repo_root / "schema" / "common"),
                "web_ui_dir": str(ctx.binary_path.parent.parent / "web_ui"),
                "web_ui_vendor_dir": str(ctx.binary_path.parent.parent / "web_ui" / "vendor"),
                "state_dir": str(ctx.webshotd_state_dir),
            },
            sort_keys=True,
        ),
    )

    pg_mode, s3_mode = _dependency_modes(ctx)
    waits = ""
    if pg_mode == "local":
        waits += wait_script(_postgres_ready_cmd(ctx))
    if s3_mode == "local":
        waits += wait_script(_seaweedfs_ready_cmd(ctx))

    run_cmd: list[str | Path] = [
        "env",
        "-u",
        "CPU_LIMIT",
        "-u",
        "DEPLOY_VCPU_LIMIT",
        "-u",
        "VCPU_LIMIT",
    ]
    if ctx.runtime_ld_library_path:
        run_cmd.append(f"LD_LIBRARY_PATH={ctx.runtime_ld_library_path}")
    run_cmd.extend(
        [
            str(ctx.binary_path),
            "--config",
            str(ctx.repo_root / "webshotd/config/static_config.yaml"),
            "--config_vars",
            str(ctx.runtime_config_vars_path),
            "--config_vars_override",
            str(ctx.webshotd_config_vars_override_path),
        ]
    )

    return ServiceScripts(
        check_cmd=_webshotd_ready_cmd(ctx),
        run_cmd=run_cmd,
        cwd=ctx.repo_root / "webshotd",
        preamble=waits,
    )


def _write_service(spec: ServiceSpec, scripts: ServiceScripts) -> None:
    (spec.service_dir / "data").mkdir(parents=True, exist_ok=True)
    write_executable(spec.service_dir / "finish", finish_script())
    write_executable(spec.service_dir / "data/check", _check_script(scripts.check_cmd))
    write_executable(
        spec.service_dir / "run",
        (
            "#!/bin/sh\n"
            f"exec >>{shell_quote(spec.log_file)} 2>&1\n"
            f"cd {shell_quote(scripts.cwd)}\n"
            f"{scripts.preamble}exec {shell_join(scripts.run_cmd)}\n"
        ),
    )


def _check_script(cmd: list[str | Path]) -> str:
    return f"#!/bin/sh\nexec {shell_join(cmd)}\n"


def _generate_local_fixture_pki(ctx: RuntimeUpContext) -> None:
    need_cmd("openssl")
    need_cmd("certutil")

    if ctx.test_pki_dir.exists():
        shutil.rmtree(ctx.test_pki_dir)
    ctx.test_pki_dir.mkdir(parents=True, exist_ok=True)
    ctx.nginx_payload_dir.mkdir(parents=True, exist_ok=True)

    ca_config_path = ctx.test_pki_dir / "trusted_root_ca.cnf"
    trusted_leaf_config_path = ctx.test_pki_dir / "trusted_test_target.cnf"
    untrusted_leaf_config_path = ctx.test_pki_dir / "untrusted_test_target.cnf"
    trusted_csr_path = ctx.test_pki_dir / "trusted_test_target.csr"
    trusted_serial_path = ctx.test_pki_dir / "trusted_root_ca.srl"
    untrusted_csr_path = ctx.test_pki_dir / "untrusted_test_target.csr"

    _write_openssl_config(
        ca_config_path,
        common_name=TRUSTED_CA_COMMON_NAME,
        dns_names=[],
        is_ca=True,
    )
    _write_openssl_config(
        trusted_leaf_config_path,
        common_name=TEST_HOST,
        dns_names=[TEST_HOST, TEST_ASSET_HOST],
        is_ca=False,
    )
    _write_openssl_config(
        untrusted_leaf_config_path,
        common_name=UNTRUSTED_TEST_HOST,
        dns_names=[UNTRUSTED_TEST_HOST],
        is_ca=False,
    )

    run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-sha256",
            "-days",
            "3650",
            "-nodes",
            "-keyout",
            str(ctx.trusted_ca_key_path),
            "-out",
            str(ctx.trusted_ca_cert_path),
            "-config",
            str(ca_config_path),
        ],
        timeout_sec=30.0,
    )
    run(
        [
            "openssl",
            "req",
            "-new",
            "-newkey",
            "rsa:2048",
            "-sha256",
            "-nodes",
            "-keyout",
            str(ctx.trusted_key_path),
            "-out",
            str(trusted_csr_path),
            "-config",
            str(trusted_leaf_config_path),
        ],
        timeout_sec=30.0,
    )
    run(
        [
            "openssl",
            "x509",
            "-req",
            "-in",
            str(trusted_csr_path),
            "-CA",
            str(ctx.trusted_ca_cert_path),
            "-CAkey",
            str(ctx.trusted_ca_key_path),
            "-CAcreateserial",
            "-CAserial",
            str(trusted_serial_path),
            "-out",
            str(ctx.trusted_cert_path),
            "-days",
            "3650",
            "-sha256",
            "-extensions",
            "v3_req",
            "-extfile",
            str(trusted_leaf_config_path),
        ],
        timeout_sec=30.0,
    )
    run(
        [
            "openssl",
            "req",
            "-new",
            "-newkey",
            "rsa:2048",
            "-sha256",
            "-nodes",
            "-keyout",
            str(ctx.untrusted_key_path),
            "-out",
            str(untrusted_csr_path),
            "-config",
            str(untrusted_leaf_config_path),
        ],
        timeout_sec=30.0,
    )
    run(
        [
            "openssl",
            "x509",
            "-req",
            "-in",
            str(untrusted_csr_path),
            "-signkey",
            str(ctx.untrusted_key_path),
            "-out",
            str(ctx.untrusted_cert_path),
            "-days",
            "3650",
            "-sha256",
            "-extensions",
            "v3_req",
            "-extfile",
            str(untrusted_leaf_config_path),
        ],
        timeout_sec=30.0,
    )

    ctx.trusted_nssdb_dir.mkdir(parents=True, exist_ok=True)
    run(
        ["certutil", "-N", "-d", f"sql:{ctx.trusted_nssdb_dir}", "--empty-password"],
        timeout_sec=30.0,
    )
    run(
        [
            "certutil",
            "-A",
            "-d",
            f"sql:{ctx.trusted_nssdb_dir}",
            "-n",
            TRUSTED_CA_COMMON_NAME,
            "-t",
            "C,,",
            "-i",
            str(ctx.trusted_ca_cert_path),
        ],
        timeout_sec=30.0,
    )


def _write_openssl_config(
    path: Path,
    *,
    common_name: str,
    dns_names: list[str],
    is_ca: bool,
) -> None:
    lines = [
        "[req]",
        "distinguished_name = req_distinguished_name",
        "prompt = no",
        "",
        "[req_distinguished_name]",
        f"CN = {common_name}",
        "",
    ]
    if is_ca:
        lines[3:3] = ["x509_extensions = v3_ca"]
        lines.extend(
            [
                "[v3_ca]",
                "basicConstraints = critical,CA:TRUE",
                "keyUsage = critical,keyCertSign,cRLSign",
                "subjectKeyIdentifier = hash",
                "authorityKeyIdentifier = keyid:always,issuer",
            ]
        )
    else:
        lines.extend(
            [
                "[v3_req]",
                "basicConstraints = CA:FALSE",
                "keyUsage = digitalSignature,keyEncipherment",
                "extendedKeyUsage = serverAuth",
                "subjectAltName = @alt_names",
                "",
                "[alt_names]",
            ]
        )
        lines.extend(
            f"DNS.{index} = {dns_name}" for index, dns_name in enumerate(dns_names, start=1)
        )
    write_text(path, "\n".join(lines) + "\n")


def _render_test_target_nginx_config(ctx: RuntimeUpContext) -> None:
    content = (ctx.repo_root / "nginx" / "nginx_test.conf").read_text(encoding="utf-8")
    for placeholder, value in {
        "__PAYLOAD_DIR__": str(ctx.nginx_payload_dir),
        "__TRUSTED_CERT__": str(ctx.trusted_cert_path),
        "__TRUSTED_KEY__": str(ctx.trusted_key_path),
        "__UNTRUSTED_CERT__": str(ctx.untrusted_cert_path),
        "__UNTRUSTED_KEY__": str(ctx.untrusted_key_path),
    }.items():
        content = content.replace(placeholder, value)
    write_text(ctx.generated_nginx_config_path, content)
