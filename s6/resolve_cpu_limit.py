#!/usr/bin/env python3
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

from s6.common import ToolError, die


def cpu_limit_is_zero(limit: str) -> bool:
    return re.fullmatch(r"0+([.][0]+)?c", limit) is not None


def read_trimmed(path: Path) -> str | None:
    try:
        data = path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return None
    data = data.strip()
    return data or None


def cpuset_cpu_count(cpuset_raw: str) -> int | None:
    cpuset = re.sub(r"\s+", "", cpuset_raw)
    if not cpuset:
        return None

    count = 0
    for part in cpuset.split(","):
        if re.fullmatch(r"[0-9]+", part):
            count += 1
            continue

        match = re.fullmatch(r"([0-9]+)-([0-9]+)", part)
        if match is None:
            return None
        start = int(match.group(1))
        end = int(match.group(2))
        if end < start:
            return None
        count += end - start + 1

    return count if count > 0 else None


def infer_cpu_threads() -> int | None:
    threads: int | None = None
    hw_threads = os.cpu_count() or 0

    cgroup_v2_path = None
    cgroup_raw = read_trimmed(Path("/proc/self/cgroup"))
    if cgroup_raw:
        for line in cgroup_raw.splitlines():
            parts = line.split(":", 2)
            if len(parts) == 3 and parts[0] == "0" and parts[1] == "":
                cgroup_v2_path = parts[2]
                break

    if cgroup_v2_path:
        cg = Path("/sys/fs/cgroup") / cgroup_v2_path.lstrip("/")
        if cg.is_dir():
            cpu_max = read_trimmed(cg / "cpu.max")
            if cpu_max:
                fields = cpu_max.split()
                if len(fields) == 2 and fields[0].isdigit() and fields[1].isdigit():
                    quota = int(fields[0])
                    period = int(fields[1])
                    if quota > 0 and period > 0:
                        threads = (quota + period - 1) // period

            cpuset_effective = read_trimmed(cg / "cpuset.cpus.effective")
            if cpuset_effective:
                cpuset_threads = cpuset_cpu_count(cpuset_effective)
                if cpuset_threads and cpuset_threads > 0:
                    threads = min(threads, cpuset_threads) if threads else cpuset_threads

    if threads is None:
        cpu_cg_path = None
        if cgroup_raw:
            for line in cgroup_raw.splitlines():
                parts = line.split(":", 2)
                if len(parts) != 3:
                    continue
                controllers = parts[1].split(",") if parts[1] else []
                if "cpu" in controllers:
                    cpu_cg_path = parts[2]
                    break
        if cpu_cg_path:
            candidates = [
                Path("/sys/fs/cgroup/cpu") / cpu_cg_path.lstrip("/"),
                Path("/sys/fs/cgroup/cpu,cpuacct") / cpu_cg_path.lstrip("/"),
            ]
            cpu_mount = next((path for path in candidates if path.is_dir()), None)
            if cpu_mount is not None:
                quota_us = read_trimmed(cpu_mount / "cpu.cfs_quota_us")
                period_us = read_trimmed(cpu_mount / "cpu.cfs_period_us")
                if (
                    quota_us
                    and period_us
                    and re.fullmatch(r"-?[0-9]+", quota_us)
                    and period_us.isdigit()
                ):
                    quota = int(quota_us)
                    period = int(period_us)
                    if quota > 0 and period > 0:
                        threads = (quota + period - 1) // period

    if threads is None and hw_threads > 0:
        threads = hw_threads
    if threads is not None and hw_threads > 0 and threads > hw_threads:
        threads = hw_threads
    return threads if threads and threads > 0 else None


def resolve_cpu_limit() -> str:
    cpu_limit = os.environ.get("CPU_LIMIT", "")
    if cpu_limit:
        if re.fullmatch(r"[0-9]+([.][0-9]+)?c", cpu_limit) is None:
            die(f"CPU_LIMIT must look like '4c' or '0.5c', got: '{cpu_limit}'", exit_code=2)
        if cpu_limit_is_zero(cpu_limit):
            die(f"CPU_LIMIT must be > 0, got: '{cpu_limit}'", exit_code=2)
        return cpu_limit

    deploy_vcpu = os.environ.get("DEPLOY_VCPU_LIMIT", "")
    if deploy_vcpu:
        if re.fullmatch(r"[0-9]+([.][0-9]+)?", deploy_vcpu) is None:
            die(
                f"DEPLOY_VCPU_LIMIT must be numeric millicores (e.g. '4000'), got: '{deploy_vcpu}'",
                exit_code=2,
            )
        if re.fullmatch(r"0+([.][0]+)?", deploy_vcpu) is not None:
            die(f"DEPLOY_VCPU_LIMIT must be > 0, got: '{deploy_vcpu}'", exit_code=2)
        return ""

    threads = infer_cpu_threads()
    if threads is None:
        die(
            "Failed to infer CPU thread count. Set CPU_LIMIT (e.g. '4c') or "
            "DEPLOY_VCPU_LIMIT (e.g. '4000').",
            exit_code=2,
        )
    return f"{threads}c"


def main() -> int:
    try:
        print(resolve_cpu_limit())
        return 0
    except ToolError as e:
        if e.message:
            print(e.message, file=sys.stderr)
        return e.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
