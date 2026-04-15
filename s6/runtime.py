from __future__ import annotations

import argparse
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

from s6.common import ToolError
from s6.runtime_context import build_inspect_context, build_state_context, build_up_context
from s6.runtime_setup import ensure_dev_bucket, prepare_runtime
from s6.runtime_supervisor import (
    check,
    logs,
    stack_healthy,
    start_supervisor,
    status,
    stop_supervisor,
    supervisor_matches_profile,
    supervisor_running,
)
from s6.runtime_support import format_runtime_message, report, runtime_die

MANAGED_CGROUP_PREFIX = "webshotd"
MANAGED_CGROUP_SUBGROUP = "service"
DELEGATED_CGROUP_CONTROLLERS = ("cpu", "memory")


@dataclass(frozen=True)
class _ManagedCgroupRootState:
    root: Path
    subtree_control_path: Path
    controllers: set[str]
    subtree_control: set[str]


def _current_cgroup_v2_relative_path() -> str:
    try:
        raw = Path("/proc/self/cgroup").read_text(encoding="utf-8")
    except FileNotFoundError as e:
        raise ToolError(message="Missing /proc/self/cgroup", exit_code=1) from e

    for line in raw.splitlines():
        parts = line.split(":", 2)
        if len(parts) == 3 and parts[0] == "0" and parts[1] == "":
            path = parts[2].strip()
            if path:
                return path
            break
    runtime_die("Failed to determine cgroup v2 path from /proc/self/cgroup", exit_code=1)


def _current_cgroup_v2_dir() -> Path:
    relative = _current_cgroup_v2_relative_path()
    if not relative.startswith("/"):
        runtime_die(f"Invalid cgroup v2 path: {relative}", exit_code=1)
    return Path("/sys/fs/cgroup") / relative.lstrip("/")


def _managed_app_slice_dir() -> tuple[str, Path]:
    uid = os.getuid()
    base_user_slice = f"/user.slice/user-{uid}.slice/"
    app_slice_dir = (
        Path("/sys/fs/cgroup")
        / f"user.slice/user-{uid}.slice"
        / f"user@{uid}.service"
        / "app.slice"
    )
    return base_user_slice, app_slice_dir


def _managed_scope_prefix(base_user_slice: str, *, uid: int) -> str:
    return f"{base_user_slice}user@{uid}.service/app.slice/{MANAGED_CGROUP_PREFIX}-"


def _managed_scope_service_dir(current_cgroup: str) -> Path | None:
    uid = os.getuid()
    base_user_slice, _ = _managed_app_slice_dir()
    prefix = _managed_scope_prefix(base_user_slice, uid=uid)
    suffix = f"/{MANAGED_CGROUP_SUBGROUP}"
    if not current_cgroup.startswith(prefix) or not current_cgroup.endswith(suffix):
        return None
    return Path("/sys/fs/cgroup") / current_cgroup.lstrip("/")


def _record_managed_cgroup_root(ctx, root: Path) -> None:
    ctx.state_dir.mkdir(parents=True, exist_ok=True)
    ctx.managed_cgroup_root_file.write_text(str(root) + "\n", encoding="utf-8")


def _enable_managed_cgroup_root_controllers(state: _ManagedCgroupRootState) -> None:
    _require_cgroup_controllers(state.controllers, label="controllers")
    enable = [
        f"+{name}" for name in DELEGATED_CGROUP_CONTROLLERS if name not in state.subtree_control
    ]
    if not enable:
        return

    try:
        state.subtree_control_path.write_text(" ".join(enable) + "\n", encoding="utf-8")
    except OSError as e:
        raise ToolError(
            message=(
                f"failed to enable managed subtree controllers at {state.subtree_control_path}: {e}"
            ),
            exit_code=1,
        ) from e


def _cleanup_managed_cgroup_root(path: Path, *, ignore_errors: bool = False) -> None:
    service_dir = path / MANAGED_CGROUP_SUBGROUP
    for target in [service_dir, path]:
        try:
            target.rmdir()
        except FileNotFoundError:
            continue
        except OSError:
            if not ignore_errors:
                raise


def _enter_managed_cgroup_subgroup(ctx) -> None:
    current_cgroup = _current_cgroup_v2_relative_path()
    current_service_dir = _managed_scope_service_dir(current_cgroup)
    if current_service_dir is not None:
        state = _read_managed_cgroup_root_state(current_service_dir)
        _enable_managed_cgroup_root_controllers(state)
        _assert_managed_cgroup_scope()
        _record_managed_cgroup_root(ctx, state.root)
        return

    uid = os.getuid()
    base_user_slice, app_slice_dir = _managed_app_slice_dir()
    if not current_cgroup.startswith(base_user_slice):
        runtime_die(
            (
                f"must run inside user-{uid}.slice so it can stage "
                "managed cgroups under app.slice; "
                f"current cgroup is {current_cgroup}"
            ),
            exit_code=1,
        )

    controllers_path = app_slice_dir / "cgroup.controllers"
    subtree_control_path = app_slice_dir / "cgroup.subtree_control"
    if not controllers_path.is_file() or not subtree_control_path.is_file():
        runtime_die(f"managed app.slice is not available: {app_slice_dir}", exit_code=1)

    managed_root = app_slice_dir / f"{MANAGED_CGROUP_PREFIX}-{os.getpid()}.scope"
    service_dir = managed_root / MANAGED_CGROUP_SUBGROUP
    service_dir.mkdir(parents=True, exist_ok=False)
    try:
        state = _read_managed_cgroup_root_state(service_dir)
        _enable_managed_cgroup_root_controllers(state)
        try:
            (service_dir / "cgroup.procs").write_text(f"{os.getpid()}\n", encoding="utf-8")
        except OSError as e:
            raise ToolError(
                message=(
                    "failed to move startup process into managed "
                    f"cgroup subgroup {service_dir}: {e}"
                ),
                exit_code=1,
            ) from e

        if _current_cgroup_v2_dir() != service_dir:
            runtime_die(
                f"startup process did not enter managed subgroup '{MANAGED_CGROUP_SUBGROUP}'",
                exit_code=1,
            )
        _assert_managed_cgroup_scope()
        _record_managed_cgroup_root(ctx, managed_root)
    except Exception:
        _cleanup_managed_cgroup_root(managed_root, ignore_errors=True)
        raise


def _assert_managed_cgroup_scope() -> None:
    state = _require_managed_cgroup_subgroup()
    _require_cgroup_controllers(state.controllers, label="controllers")
    _require_cgroup_controllers(state.subtree_control, label="subtree controllers")


def _require_managed_cgroup_subgroup() -> _ManagedCgroupRootState:
    current_cgroup = _current_cgroup_v2_dir()
    if current_cgroup.name != MANAGED_CGROUP_SUBGROUP:
        runtime_die(
            (
                "expected to run inside managed cgroup subgroup "
                f"'{MANAGED_CGROUP_SUBGROUP}', got {current_cgroup}"
            ),
            exit_code=1,
        )
    return _read_managed_cgroup_root_state(current_cgroup)


def _read_managed_cgroup_root_state(current_cgroup: Path) -> _ManagedCgroupRootState:
    managed_root = current_cgroup.parent
    if managed_root == current_cgroup:
        runtime_die("managed cgroup root is missing", exit_code=1)

    controllers_path = managed_root / "cgroup.controllers"
    subtree_control_path = managed_root / "cgroup.subtree_control"
    if not controllers_path.is_file() or not subtree_control_path.is_file():
        runtime_die(f"managed cgroup root is not writable from {managed_root}", exit_code=1)

    return _ManagedCgroupRootState(
        root=managed_root,
        subtree_control_path=subtree_control_path,
        controllers=set(controllers_path.read_text(encoding="utf-8").split()),
        subtree_control=set(subtree_control_path.read_text(encoding="utf-8").split()),
    )


def _require_cgroup_controllers(available: set[str], *, label: str) -> None:
    missing = [name for name in DELEGATED_CGROUP_CONTROLLERS if name not in available]
    if missing:
        runtime_die(
            f"managed cgroup root is missing required {label}: " + ", ".join(missing),
            exit_code=1,
        )


def _up(ctx) -> None:
    if supervisor_running(ctx):
        if not supervisor_matches_profile(ctx):
            runtime_die(
                "stack already supervised with a different service profile; run down first",
                exit_code=1,
            )
        if not stack_healthy(ctx):
            runtime_die("stack already supervised but not healthy; run down first", exit_code=1)
        report("already running")
        if ctx.mode == "dev":
            ensure_dev_bucket(ctx)
        return

    if ctx.service_profile == "full":
        _enter_managed_cgroup_subgroup(ctx)

    start_supervisor(ctx, prepare_runtime(ctx))
    if ctx.mode == "dev":
        ensure_dev_bucket(ctx)


def _down(ctx) -> None:
    stop_supervisor(ctx)
    if ctx.managed_cgroup_root_file.exists():
        raw = ctx.managed_cgroup_root_file.read_text(encoding="utf-8").strip()
        if raw:
            _cleanup_managed_cgroup_root(Path(raw))
    if ctx.state_dir.exists():
        shutil.rmtree(ctx.state_dir)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="s6.runtime")
    subparsers = parser.add_subparsers(dest="action", required=True)

    up_parser = subparsers.add_parser("up")
    up_parser.add_argument("--mode", required=True, choices=["dev", "prodlike"])
    up_parser.add_argument("--service-profile", choices=["full", "test_infra"], default="full")
    up_parser.add_argument("--binary-path", required=True)
    up_parser.add_argument("--config-vars-source", required=True)
    up_parser.add_argument("--runtime-ld-library-path", required=True)

    down_parser = subparsers.add_parser("down")
    down_parser.add_argument("--mode", required=True, choices=["dev", "prodlike"])

    for action in ["status", "logs", "check"]:
        action_parser = subparsers.add_parser(action)
        action_parser.add_argument("--mode", required=True, choices=["dev", "prodlike"])
        action_parser.add_argument(
            "--service-profile",
            choices=["full", "test_infra"],
            default="full",
        )

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(list(sys.argv[1:] if argv is None else argv))

    try:
        if args.action == "up":
            ctx = build_up_context(
                mode=args.mode,
                service_profile=args.service_profile,
                binary_path=args.binary_path,
                config_vars_source=args.config_vars_source,
                runtime_ld_library_path=args.runtime_ld_library_path,
            )
            _up(ctx)
        elif args.action == "down":
            _down(build_state_context(mode=args.mode))
        elif args.action == "status":
            status(build_inspect_context(mode=args.mode, service_profile=args.service_profile))
        elif args.action == "logs":
            logs(build_inspect_context(mode=args.mode, service_profile=args.service_profile))
        elif args.action == "check":
            return check(
                build_inspect_context(mode=args.mode, service_profile=args.service_profile)
            )
        else:
            raise AssertionError("unreachable")
        return 0
    except ToolError as e:
        if e.message:
            print(format_runtime_message(e.message), file=sys.stderr)
        return e.exit_code
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
