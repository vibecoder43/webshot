from __future__ import annotations

import os
import sys
import time
from contextlib import suppress
from pathlib import Path

from compose_tools.common import ToolError, die, need_cmd, run
from compose_tools.podman_compose import (
    ContainerState,
    compose,
    podman_inspect_state,
    require_podman_compose,
    wait_healthy,
    wait_running,
)
from compose_tools.s3_bucket import ensure_s3_bucket_exists


def ensure_networks() -> None:
    need_cmd("podman")

    def ensure_network(name: str, want_internal: str) -> None:
        inspect = run(["podman", "network", "inspect", name], check=False, capture=True)
        if inspect.returncode == 0:
            internal = run(
                ["podman", "network", "inspect", "-f", "{{.Internal}}", name],
                capture=True,
                check=False,
            ).stdout.strip()
            if internal != want_internal:
                print(
                    f"Podman network '{name}' has Internal='{internal}', "
                    f"expected '{want_internal}'.",
                    file=sys.stderr,
                )
                print("Bring down containers that use it and recreate it:", file=sys.stderr)
                print(f"  podman network rm '{name}'", file=sys.stderr)
                if want_internal == "true":
                    print(f"  podman network create --internal '{name}'", file=sys.stderr)
                else:
                    print(f"  podman network create '{name}'", file=sys.stderr)
                raise ToolError(message="", exit_code=1)
            return

        if want_internal == "true":
            run(["podman", "network", "create", "--internal", name])
        else:
            run(["podman", "network", "create", name])

    ensure_network("crawler_net", "true")
    ensure_network("egress_net", "false")


def infra_ready(*, mode: str, verbose: bool = False) -> bool:
    need_cmd("podman")

    if mode == "dev":
        names = ["egress_proxy", "servicedb", "seaweedfs", "scalar", "reverse_proxy", "test_target"]
    elif mode == "prodlike":
        names = ["egress_proxy", "servicedb"]
    else:
        die("mode must be 'dev' or 'prodlike'", exit_code=2)

    for name in names:
        state: ContainerState | None = None
        try:
            state = podman_inspect_state(name)
        except ToolError:
            if verbose:
                print(f"infra: {name}: missing", file=sys.stderr)
                run(
                    [
                        "podman",
                        "ps",
                        "-a",
                        "--filter",
                        f"name=^{name}$",
                        "--format",
                        "infra: {{.Names}}: {{.Status}}",
                    ],
                    check=False,
                )
            return False

        if state.status != "running":
            if verbose:
                print(f"infra: {name}: not running (status='{state.status}')", file=sys.stderr)
                run(
                    [
                        "podman",
                        "ps",
                        "-a",
                        "--filter",
                        f"name=^{name}$",
                        "--format",
                        "infra: {{.Names}}: {{.Status}}",
                    ],
                    check=False,
                )
                run(["podman", "logs", "--tail=50", name], check=False)
            return False

        if state.health and state.health != "healthy":
            if verbose:
                print(f"infra: {name}: health='{state.health}'", file=sys.stderr)
                run(["podman", "logs", "--tail=50", name], check=False)
            return False

    return True


def infra_status(*, mode: str) -> None:
    print(f"Mode: {mode}")
    if infra_ready(mode=mode):
        print("Infra: ready")
    else:
        print("Infra: not ready")
        infra_ready(mode=mode, verbose=True)


def assert_servicedb_timezone_utc(*, container_name: str = "servicedb") -> None:
    tz_raw = run(
        ["podman", "exec", container_name, "psql", "-U", "postgres", "-tAqc", "SHOW TimeZone;"],
        capture=True,
        check=False,
    )
    if tz_raw.returncode != 0:
        print(
            f"Failed to read Postgres TimeZone from container '{container_name}':", file=sys.stderr
        )
        print(tz_raw.stdout, file=sys.stderr)
        print(tz_raw.stderr, file=sys.stderr)
        raise ToolError(message="Failed to read Postgres TimeZone", exit_code=1)

    log_tz_raw = run(
        ["podman", "exec", container_name, "psql", "-U", "postgres", "-tAqc", "SHOW log_timezone;"],
        capture=True,
        check=False,
    )
    if log_tz_raw.returncode != 0:
        print(
            f"Failed to read Postgres log_timezone from container '{container_name}':",
            file=sys.stderr,
        )
        print(log_tz_raw.stdout, file=sys.stderr)
        print(log_tz_raw.stderr, file=sys.stderr)
        raise ToolError(message="Failed to read Postgres log_timezone", exit_code=1)

    tz = "".join(tz_raw.stdout.split())
    log_tz = "".join(log_tz_raw.stdout.split())
    if not tz or not log_tz:
        raise ToolError(
            message=f"Failed to read Postgres timezone settings from container '{container_name}'.",
            exit_code=1,
        )
    if tz != "UTC":
        raise ToolError(
            message=f"Postgres TimeZone must be UTC, got '{tz}' (container='{container_name}').",
            exit_code=1,
        )
    if log_tz != "UTC":
        raise ToolError(
            message=(
                f"Postgres log_timezone must be UTC, got '{log_tz}' (container='{container_name}')."
            ),
            exit_code=1,
        )


def infra_down_compose(*, compose_dir: Path, compose_file: str) -> None:
    require_podman_compose()
    timeout_sec = int(os.environ.get("INFRA_DOWN_TIMEOUT_SEC", "90"))
    try:
        proc = run(
            ["podman", "compose", "--in-pod", "true", "-f", compose_file, "down"],
            cwd=compose_dir,
            timeout_sec=float(timeout_sec),
            capture=True,
            check=False,
        )
    except ToolError:
        proc = None
    else:
        if proc.returncode == 0:
            return

    pod_inspect = run(["podman", "pod", "inspect", "pod_compose"], check=False, capture=True)
    if pod_inspect.returncode != 0:
        print("Infra is already down (pod 'pod_compose' not found).", file=sys.stderr)
        return

    print(
        f"Warning: podman compose down failed/timed out after {timeout_sec}s; "
        "forcing pod removal: pod_compose",
        file=sys.stderr,
    )
    run(["podman", "pod", "rm", "-f", "pod_compose"], check=False)
    run(["podman", "pod", "inspect", "pod_compose"], check=False)


def ensure_servicedb_timezone_utc_or_down(
    *, compose_dir: Path, compose_file: str, container_name: str = "servicedb"
) -> None:
    try:
        assert_servicedb_timezone_utc(container_name=container_name)
    except ToolError:
        with suppress(ToolError):
            infra_down_compose(compose_dir=compose_dir, compose_file=compose_file)
        raise


def _squid_load(mode: str) -> None:
    if mode == "dev":
        cmd = "squid_load_dev"
    elif mode == "prodlike":
        cmd = "squid_load_prodlike"
    else:
        die("mode must be 'dev' or 'prodlike'", exit_code=2)
    need_cmd(cmd)
    run([cmd])


def _ensure_s3_bucket_dev(*, repo_root: Path) -> None:
    secdist_path = repo_root / "secret/test_secdist.json"
    endpoint = "localhost:8333"
    bucket = "webshot"
    timeout_sec = 30
    retry_delay_sec = 1

    deadline = time.monotonic() + timeout_sec
    while True:
        try:
            ensure_s3_bucket_exists(secrets_path=secdist_path, endpoint=endpoint, bucket=bucket)
            return
        except ToolError as e:
            if e.exit_code == 2:
                raise
        except Exception:
            pass
        if time.monotonic() >= deadline:
            raise ToolError(
                message=(
                    f"Timed out ensuring S3 bucket exists: bucket='{bucket}' endpoint='{endpoint}'"
                ),
                exit_code=1,
            )
        time.sleep(retry_delay_sec)


def infra_up(*, mode: str, compose_dir: Path, repo_root: Path) -> None:
    ensure_networks()
    _squid_load(mode)

    compose_file = "infra_dev.yaml" if mode == "dev" else "infra_prodlike.yaml"
    compose(["--in-pod", "true", "-f", compose_file, "up", "-d"], cwd=compose_dir)

    wait_healthy("egress_proxy", 120)
    wait_healthy("servicedb", 120)
    ensure_servicedb_timezone_utc_or_down(
        compose_dir=compose_dir, compose_file=compose_file, container_name="servicedb"
    )

    if mode == "dev":
        wait_healthy("seaweedfs", 120)
        _ensure_s3_bucket_dev(repo_root=repo_root)
        wait_running("scalar", 120)
        wait_running("reverse_proxy", 120)
        wait_running("test_target", 120)


def infra_down(*, mode: str, compose_dir: Path) -> None:
    compose_file = "infra_dev.yaml" if mode == "dev" else "infra_prodlike.yaml"
    infra_down_compose(compose_dir=compose_dir, compose_file=compose_file)
