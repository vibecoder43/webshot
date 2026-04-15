from __future__ import annotations

import argparse
import errno
import os
import shutil
import sys
import time
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
PARENT_PROCS_CGROUP_NAME = f"{MANAGED_CGROUP_PREFIX}-parent-procs"
DELEGATED_CGROUP_CONTROLLERS = ("cpu", "memory")
CGROUP_KILL_WAIT_TIMEOUT_SEC = 5.0
CGROUP_KILL_POLL_INTERVAL_SEC = 0.1
CGROUP_FS_ROOT = Path("/sys/fs/cgroup")


@dataclass(frozen=True)
class _ManagedCgroupRootState:
    root: Path
    subtree_control_path: Path
    controllers: set[str]
    subtree_control: set[str]


@dataclass(frozen=True)
class _ManagedCgroupParent:
    path: Path
    drain_processes_before_enable: bool


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
    return _cgroup_v2_dir(_current_cgroup_v2_relative_path())


def _cgroup_v2_dir(relative: str) -> Path:
    if not relative.startswith("/"):
        runtime_die(f"Invalid cgroup v2 path: {relative}", exit_code=1)
    return CGROUP_FS_ROOT / relative.lstrip("/")


def _managed_app_slice_dir() -> tuple[str, Path]:
    uid = os.getuid()
    base_user_slice = f"/user.slice/user-{uid}.slice/"
    app_slice_dir = (
        CGROUP_FS_ROOT / f"user.slice/user-{uid}.slice" / f"user@{uid}.service" / "app.slice"
    )
    return base_user_slice, app_slice_dir


def _has_cgroup_control_files(path: Path) -> bool:
    return (path / "cgroup.controllers").is_file() and (path / "cgroup.subtree_control").is_file()


def _is_managed_cgroup_root(path: Path) -> bool:
    return path.name.startswith(f"{MANAGED_CGROUP_PREFIX}-") and path.name.endswith(".scope")


def _managed_scope_service_dir(current_cgroup: str) -> Path | None:
    suffix = f"/{MANAGED_CGROUP_SUBGROUP}"
    if not current_cgroup.endswith(suffix):
        return None

    service_dir = _cgroup_v2_dir(current_cgroup)
    if not _is_managed_cgroup_root(service_dir.parent):
        return None
    return service_dir


def _managed_cgroup_parent_dir(current_cgroup: str) -> _ManagedCgroupParent:
    base_user_slice, app_slice_dir = _managed_app_slice_dir()
    if current_cgroup.startswith(base_user_slice) and _has_cgroup_control_files(app_slice_dir):
        return _ManagedCgroupParent(path=app_slice_dir, drain_processes_before_enable=False)

    current_dir = _cgroup_v2_dir(current_cgroup)
    if current_dir.name == PARENT_PROCS_CGROUP_NAME and _has_cgroup_control_files(
        current_dir.parent
    ):
        return _ManagedCgroupParent(path=current_dir.parent, drain_processes_before_enable=False)

    if current_dir == CGROUP_FS_ROOT:
        runtime_die("must run inside a delegated cgroup; current cgroup is /", exit_code=1)
    if not _has_cgroup_control_files(current_dir):
        runtime_die(
            f"managed cgroup parent is not available from current cgroup: {current_dir}",
            exit_code=1,
        )
    return _ManagedCgroupParent(path=current_dir, drain_processes_before_enable=True)


def _record_managed_cgroup_root(ctx, root: Path) -> None:
    ctx.state_dir.mkdir(parents=True, exist_ok=True)
    ctx.managed_cgroup_root_file.write_text(str(root) + "\n", encoding="utf-8")


def _enable_cgroup_controllers(state: _ManagedCgroupRootState, *, label: str) -> None:
    _require_cgroup_controllers(state.controllers, label=label)
    enable = [f"+{name}" for name in _missing_cgroup_controllers(state.subtree_control)]
    if not enable:
        return

    try:
        state.subtree_control_path.write_text(" ".join(enable) + "\n", encoding="utf-8")
    except OSError as e:
        raise ToolError(
            message=f"failed to enable {label} subtree controllers at {state.root}: {e}",
            exit_code=1,
        ) from e


def _enable_managed_cgroup_root_controllers(state: _ManagedCgroupRootState) -> None:
    _enable_cgroup_controllers(state, label="managed cgroup root")


def _prepare_managed_cgroup_parent(parent: _ManagedCgroupParent) -> None:
    state = _read_cgroup_controller_state(parent.path, label="managed cgroup parent")
    if parent.drain_processes_before_enable and _missing_cgroup_controllers(state.subtree_control):
        _drain_cgroup_processes_to_leaf(parent.path)
        state = _read_cgroup_controller_state(parent.path, label="managed cgroup parent")
    _enable_cgroup_controllers(state, label="managed cgroup parent")


def _drain_cgroup_processes_to_leaf(path: Path) -> None:
    leaf = path / PARENT_PROCS_CGROUP_NAME
    try:
        leaf.mkdir(exist_ok=True)
    except OSError as e:
        raise ToolError(
            message=f"failed to create cgroup process leaf {leaf}: {e}",
            exit_code=1,
        ) from e

    procs_path = leaf / "cgroup.procs"
    for _ in range(20):
        procs = _read_cgroup_procs(path)
        if not procs:
            return
        for pid in procs:
            try:
                procs_path.write_text(f"{pid}\n", encoding="utf-8")
            except OSError as e:
                if e.errno == errno.ESRCH or not (Path("/proc") / pid).exists():
                    continue
                raise ToolError(
                    message=f"failed to move process {pid} into cgroup process leaf {leaf}: {e}",
                    exit_code=1,
                ) from e

    procs = _read_cgroup_procs(path)
    detail = f" (pids: {', '.join(procs)})" if procs else ""
    runtime_die(f"managed cgroup parent stayed populated after draining: {path}{detail}")


def _validate_managed_cgroup_root_path(path: Path) -> Path:
    resolved = path.resolve(strict=False)
    if resolved == CGROUP_FS_ROOT or CGROUP_FS_ROOT not in resolved.parents:
        runtime_die(
            f"refusing to clean managed cgroup root outside cgroupfs: {path}",
            exit_code=1,
        )
    if not _is_managed_cgroup_root(resolved):
        runtime_die(
            f"refusing to clean unexpected managed cgroup root name: {path}",
            exit_code=1,
        )
    if resolved.exists() and not _has_cgroup_control_files(resolved):
        runtime_die(
            f"refusing to clean unexpected managed cgroup root without cgroup files: {path}",
            exit_code=1,
        )
    return resolved


def _iter_managed_cgroup_dirs_deepest_first(path: Path) -> list[Path]:
    dirs = [Path(root) for root, _, _ in os.walk(path)]
    dirs.sort(key=lambda current: (-len(current.parts), str(current)))
    return dirs


def _read_cgroup_populated(path: Path) -> bool:
    events_path = path / "cgroup.events"
    try:
        raw = events_path.read_text(encoding="utf-8")
    except FileNotFoundError as e:
        raise ToolError(message=f"missing cgroup events file at {events_path}", exit_code=1) from e
    except OSError as e:
        raise ToolError(
            message=f"failed to read cgroup events at {events_path}: {e}",
            exit_code=1,
        ) from e

    for line in raw.splitlines():
        key, _, value = line.partition(" ")
        if key == "populated":
            if value == "0":
                return False
            if value == "1":
                return True
            break
    runtime_die(f"missing populated state in cgroup events at {events_path}", exit_code=1)


def _read_cgroup_procs(path: Path) -> list[str]:
    procs_path = path / "cgroup.procs"
    try:
        return [line for line in procs_path.read_text(encoding="utf-8").splitlines() if line]
    except FileNotFoundError as e:
        raise ToolError(message=f"missing cgroup process file at {procs_path}", exit_code=1) from e
    except OSError as e:
        raise ToolError(
            message=f"failed to read cgroup processes at {procs_path}: {e}",
            exit_code=1,
        ) from e


def _kill_managed_cgroup_dir(path: Path) -> None:
    kill_path = path / "cgroup.kill"
    procs = _read_cgroup_procs(path)
    try:
        kill_path.write_text("1\n", encoding="utf-8")
    except FileNotFoundError as e:
        raise ToolError(
            message=f"missing cgroup kill file at {kill_path} while draining {path}",
            exit_code=1,
        ) from e
    except OSError as e:
        detail = f" (pids: {', '.join(procs)})" if procs else ""
        raise ToolError(
            message=f"failed to kill lingering processes in managed cgroup {path}{detail}: {e}",
            exit_code=1,
        ) from e


def _drain_managed_cgroup_dir(path: Path) -> None:
    if not _read_cgroup_populated(path):
        return

    _kill_managed_cgroup_dir(path)
    deadline = time.monotonic() + CGROUP_KILL_WAIT_TIMEOUT_SEC
    while time.monotonic() < deadline:
        if not _read_cgroup_populated(path):
            return
        time.sleep(CGROUP_KILL_POLL_INTERVAL_SEC)

    procs = _read_cgroup_procs(path)
    detail = f" (pids: {', '.join(procs)})" if procs else ""
    runtime_die(f"managed cgroup stayed populated after kill: {path}{detail}", exit_code=1)


def _remove_managed_cgroup_dir(path: Path) -> None:
    try:
        path.rmdir()
    except FileNotFoundError:
        return
    except OSError as e:
        raise ToolError(message=f"failed to remove managed cgroup {path}: {e}", exit_code=1) from e


def _cleanup_managed_cgroup_root(path: Path, *, ignore_errors: bool = False) -> None:
    try:
        path = _validate_managed_cgroup_root_path(path)
        if not path.exists():
            return

        for current in _iter_managed_cgroup_dirs_deepest_first(path):
            _drain_managed_cgroup_dir(current)
            _remove_managed_cgroup_dir(current)
    except (ToolError, OSError):
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

    parent = _managed_cgroup_parent_dir(current_cgroup)
    _prepare_managed_cgroup_parent(parent)

    managed_root = parent.path / f"{MANAGED_CGROUP_PREFIX}-{os.getpid()}.scope"
    service_dir = managed_root / MANAGED_CGROUP_SUBGROUP
    try:
        try:
            service_dir.mkdir(parents=True, exist_ok=False)
        except OSError as e:
            raise ToolError(
                message=f"failed to create managed cgroup subgroup {service_dir}: {e}",
                exit_code=1,
            ) from e

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
    _require_cgroup_controllers(state.controllers, label="managed cgroup root")
    _require_cgroup_controllers(state.subtree_control, label="managed cgroup root subtree")


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

    return _read_cgroup_controller_state(managed_root, label="managed cgroup root")


def _read_cgroup_controller_state(root: Path, *, label: str) -> _ManagedCgroupRootState:
    controllers_path = root / "cgroup.controllers"
    subtree_control_path = root / "cgroup.subtree_control"
    if not controllers_path.is_file() or not subtree_control_path.is_file():
        runtime_die(f"{label} is not writable from {root}", exit_code=1)

    return _ManagedCgroupRootState(
        root=root,
        subtree_control_path=subtree_control_path,
        controllers=set(controllers_path.read_text(encoding="utf-8").split()),
        subtree_control=set(subtree_control_path.read_text(encoding="utf-8").split()),
    )


def _require_cgroup_controllers(available: set[str], *, label: str) -> None:
    missing = _missing_cgroup_controllers(available)
    if missing:
        runtime_die(
            f"{label} is missing required controllers: " + ", ".join(missing),
            exit_code=1,
        )


def _missing_cgroup_controllers(available: set[str]) -> list[str]:
    return [name for name in DELEGATED_CGROUP_CONTROLLERS if name not in available]


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
