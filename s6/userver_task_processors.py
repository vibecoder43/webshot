from __future__ import annotations

import math
import os
from typing import Any

from s6.common import CGROUP_FS_ROOT, current_cgroup_v2_relative_path, die

USERVER_DEFAULT_HW_THREADS_ESTIMATE = 512
MAIN_WORKER_THREADS_CONFIG_VAR = "$main_worker_threads"
MAIN_WORKER_THREADS_FALLBACK_DEFAULT = 4


def task_processor_config_vars(static_config: dict[str, Any]) -> dict[str, int]:
    main_threads = main_worker_threads(static_config)
    return {
        "main_worker_threads": main_threads,
        "fs_worker_threads": main_threads + 2,
    }


def fs_worker_threads(static_config: dict[str, Any]) -> int:
    return main_worker_threads(static_config) + 2


def main_worker_threads(static_config: dict[str, Any]) -> int:
    fallback = _static_main_worker_threads(static_config)
    cpu_limit = _cgroup_v2_cpu_limit()
    if cpu_limit is None:
        return fallback

    hw_threads = os.cpu_count() or USERVER_DEFAULT_HW_THREADS_ESTIMATE
    cpu = _lround(cpu_limit)
    if cpu > 0 and cpu < hw_threads * 2:
        return max(cpu, 3)
    return fallback


def _static_main_worker_threads(static_config: dict[str, Any]) -> int:
    try:
        main_task_processor = static_config["components_manager"]["task_processors"][
            "main-task-processor"
        ]
        value = main_task_processor["worker_threads"]
    except KeyError:
        die("static config is missing main-task-processor.worker_threads", exit_code=2)

    if value == MAIN_WORKER_THREADS_CONFIG_VAR:
        value = main_task_processor.get("worker_threads#fallback", MAIN_WORKER_THREADS_FALLBACK_DEFAULT)

    if not isinstance(value, int) or value <= 0:
        die(
            "main-task-processor.worker_threads must be a positive integer",
            exit_code=2,
        )
    return value


def _cgroup_v2_cpu_limit() -> float | None:
    cpu_max_path = CGROUP_FS_ROOT / current_cgroup_v2_relative_path().lstrip("/") / "cpu.max"
    try:
        raw = cpu_max_path.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        return None

    parts = raw.split()
    if len(parts) != 2 or parts[0] == "max":
        return None

    try:
        quota = int(parts[0])
        period = int(parts[1])
    except ValueError:
        return None
    if period == 0:
        return None
    return quota / period


def _lround(value: float) -> int:
    return math.floor(value + 0.5)
