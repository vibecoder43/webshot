from __future__ import annotations

import dataclasses
import datetime as dt
import json
import os
import pathlib
import resource
import time
from collections.abc import Callable
from contextlib import suppress
from urllib.parse import urlsplit

import pytest
from pytest_userver import client as userver_client
from pytest_userver.plugins import service_client as service_client_plugin

_PROFILE_SCHEMA_VERSION = 1
_PROC_CLK_TCK = os.sysconf("SC_CLK_TCK")
_DEV_STATE_DIR = pathlib.Path("/tmp/webshot/dev")
_S6_SCAN_DIR = _DEV_STATE_DIR / "s6-scan"
_PROFILE_PATH_NAME = "coarse_profile.json"
_PROFILE_TEXT_PATH_NAME = "coarse_profile.txt"
_S6_STATUS_PID_OFFSET = 28
_S6_STATUS_PID_SIZE = 4
_BUCKET_WEBSHOTD = "webshotd"
_BUCKET_CHROMIUM = "chromium"
_S6_SERVICE_BUCKET_PREFIX = "s6_service:"


@dataclasses.dataclass(frozen=True)
class _ProcInfo:
    pid: int
    ppid: int
    cpu_ms: int
    args: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class _Snapshot:
    wall_ms: int
    python_cpu_user_ms: int
    python_cpu_sys_ms: int
    cpu_ms_by_bucket: dict[str, int]


@dataclasses.dataclass
class _TestRecord:
    nodeid: str
    outcome: str = "unknown"
    wall_ms: int = 0
    python_cpu_user_ms: int = 0
    python_cpu_sys_ms: int = 0
    cpu_ms_by_bucket: dict[str, int] = dataclasses.field(default_factory=dict)
    capture_job_wall_ms: int = 0
    _counted_capture_job_ids: set[str] = dataclasses.field(default_factory=set)

    def to_json(self) -> dict[str, object]:
        return {
            "nodeid": self.nodeid,
            "outcome": self.outcome,
            "wall_ms": self.wall_ms,
            "python_cpu_user_ms": self.python_cpu_user_ms,
            "python_cpu_sys_ms": self.python_cpu_sys_ms,
            **_cpu_json_fields(self.cpu_ms_by_bucket),
            "capture_job_wall_ms": self.capture_job_wall_ms,
        }


def _s6_service_bucket(name: str) -> str:
    return f"{_S6_SERVICE_BUCKET_PREFIX}{name}"


def _bucket_service_name(bucket: str) -> str | None:
    if not bucket.startswith(_S6_SERVICE_BUCKET_PREFIX):
        return None
    return bucket[len(_S6_SERVICE_BUCKET_PREFIX) :]


def _named_cpu_ms(cpu_ms_by_bucket: dict[str, int], bucket: str) -> int:
    return cpu_ms_by_bucket.get(bucket, 0)


def _cpu_json_fields(cpu_ms_by_bucket: dict[str, int]) -> dict[str, object]:
    service_cpu_ms: dict[str, int] = {}
    for bucket, value in sorted(cpu_ms_by_bucket.items()):
        service_name = _bucket_service_name(bucket)
        if service_name is None:
            continue
        service_cpu_ms[service_name] = value
    return {
        "webshotd_cpu_ms": _named_cpu_ms(cpu_ms_by_bucket, _BUCKET_WEBSHOTD),
        "chromium_cpu_ms": _named_cpu_ms(cpu_ms_by_bucket, _BUCKET_CHROMIUM),
        "service_cpu_ms": service_cpu_ms,
    }


def _parse_proc_stat(raw: str) -> tuple[int, int, int] | None:
    close_paren = raw.rfind(")")
    if close_paren == -1:
        return None
    pid_text = raw[: raw.find(" ")].strip()
    if not pid_text:
        return None
    parts = raw[close_paren + 2 :].split()
    if len(parts) <= 12:
        return None
    try:
        pid = int(pid_text)
        ppid = int(parts[1])
        utime = int(parts[11])
        stime = int(parts[12])
    except ValueError:
        return None
    cpu_ms = ((utime + stime) * 1000) // _PROC_CLK_TCK
    return pid, ppid, cpu_ms


def _parse_cgroup_cpu_ms(cpu_stat_path: pathlib.Path) -> int | None:
    try:
        lines = cpu_stat_path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    prefix = "usage_usec "
    for line in lines:
        if not line.startswith(prefix):
            continue
        value = line[len(prefix) :].strip()
        try:
            return int(value) // 1000
        except ValueError:
            return None
    return None


def _parse_iso8601_ms(value: str) -> int | None:
    normalized = value
    if normalized.endswith("Z"):
        normalized = normalized[:-1] + "+00:00"
    try:
        parsed = dt.datetime.fromisoformat(normalized)
    except ValueError:
        return None
    return int(parsed.timestamp() * 1000)


class _CpuTracker:
    def __init__(
        self,
        *,
        name: str,
        pid_resolver: Callable[[_CoarseProfiler, dict[int, _ProcInfo]], int | None],
        include_descendants: bool,
    ) -> None:
        self.name = name
        self._pid_resolver = pid_resolver
        self._include_descendants = include_descendants
        self._current_pid: int | None = None
        self._current_pid_cpu_ms = 0
        self._completed_cpu_ms = 0

    @property
    def current_pid(self) -> int | None:
        return self._current_pid

    def snapshot_cpu_ms(
        self,
        proc_table: dict[int, _ProcInfo],
        children_by_ppid: dict[int, list[int]],
        profiler: _CoarseProfiler,
    ) -> int:
        pid = self._pid_resolver(profiler, proc_table)
        if pid is None:
            if self._current_pid is not None:
                self._completed_cpu_ms += self._current_pid_cpu_ms
                self._current_pid = None
                self._current_pid_cpu_ms = 0
            return self._completed_cpu_ms

        if self._current_pid is not None and self._current_pid != pid:
            self._completed_cpu_ms += self._current_pid_cpu_ms
            self._current_pid_cpu_ms = 0

        self._current_pid = pid
        current_cpu_ms = self._resolve_current_cpu_ms(proc_table, children_by_ppid, pid)
        if current_cpu_ms is None:
            return self._completed_cpu_ms

        self._current_pid_cpu_ms = max(self._current_pid_cpu_ms, current_cpu_ms)
        return self._completed_cpu_ms + self._current_pid_cpu_ms

    def _resolve_current_cpu_ms(
        self,
        proc_table: dict[int, _ProcInfo],
        children_by_ppid: dict[int, list[int]],
        pid: int,
    ) -> int | None:
        proc = proc_table.get(pid)
        if proc is None:
            return None
        if not self._include_descendants:
            return proc.cpu_ms

        total_cpu_ms = 0
        pending = [pid]
        while pending:
            current_pid = pending.pop()
            current_proc = proc_table.get(current_pid)
            if current_proc is None:
                continue
            total_cpu_ms += current_proc.cpu_ms
            pending.extend(children_by_ppid.get(current_pid, ()))
        return total_cpu_ms


def _resolve_s6_service_pid(
    *,
    profiler: _CoarseProfiler,
    service_name: str,
    status_path: pathlib.Path,
) -> int | None:
    try:
        raw = status_path.read_bytes()
    except FileNotFoundError:
        return None
    except OSError as exc:
        profiler.record_error_once(
            f"read_s6_status_failed_{service_name}",
            f"coarse_profile: failed to read {status_path}: {exc}",
        )
        return None

    required_len = _S6_STATUS_PID_OFFSET + _S6_STATUS_PID_SIZE
    if len(raw) < required_len:
        profiler.record_error_once(
            f"short_s6_status_{service_name}",
            f"coarse_profile: short s6 status file for {service_name}: {status_path}",
        )
        return None

    pid = int.from_bytes(
        raw[_S6_STATUS_PID_OFFSET : _S6_STATUS_PID_OFFSET + _S6_STATUS_PID_SIZE],
        byteorder="big",
        signed=False,
    )
    return pid or None


class _CoarseProfiler:
    def __init__(self, config: pytest.Config) -> None:
        self._config = config
        service_binary = getattr(config.option, "service_binary", None)
        self._service_binary = pathlib.Path(service_binary).resolve() if service_binary else None
        self._service_binary_candidates = self._build_service_binary_candidates(
            self._service_binary
        )
        basetemp_option = getattr(config.option, "basetemp", None)
        basetemp = (
            pathlib.Path(str(basetemp_option)).resolve() if basetemp_option else pathlib.Path.cwd()
        )
        self.output_path = basetemp / _PROFILE_PATH_NAME
        self.summary_path = basetemp / _PROFILE_TEXT_PATH_NAME
        self._errors: list[str] = []
        self._error_keys: set[str] = set()
        self._test_records: dict[str, _TestRecord] = {}
        self._test_order: list[str] = []
        self._test_start_snapshots: dict[str, _Snapshot] = {}
        self._current_test_nodeid: str | None = None
        self._job_owner_by_id: dict[str, str] = {}
        self._session_start_snapshot: _Snapshot | None = None
        self._session_end_snapshot: _Snapshot | None = None
        self._session_started_at = dt.datetime.now(dt.UTC)
        self._session_finished_at: dt.datetime | None = None
        self._chromium_cpu_ms = 0
        self._bucket_trackers: dict[str, _CpuTracker] = {
            _BUCKET_WEBSHOTD: _CpuTracker(
                name=_BUCKET_WEBSHOTD,
                pid_resolver=self._resolve_webshotd_pid,
                include_descendants=False,
            ),
        }

    def start_session(self) -> None:
        self._session_start_snapshot = self._snapshot()

    def finish_session(self) -> None:
        self._session_end_snapshot = self._snapshot()
        self._session_finished_at = dt.datetime.now(dt.UTC)
        summary_written = False
        try:
            self.output_path.parent.mkdir(parents=True, exist_ok=True)
            self.summary_path.write_text(self._build_summary_text(), encoding="utf-8")
            summary_written = True
        except OSError as exc:
            self.record_error_once(
                "write_text_failed",
                f"coarse_profile: failed to write {self.summary_path}: {exc}",
            )

        payload = self._build_json_payload()
        try:
            self.output_path.write_text(
                json.dumps(payload, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
        except OSError as exc:
            self.record_error_once(
                "write_json_failed",
                f"coarse_profile: failed to write {self.output_path}: {exc}",
            )
        else:
            if summary_written and self._errors:
                with suppress(OSError):
                    self.summary_path.write_text(self._build_summary_text(), encoding="utf-8")

    def start_test(self, nodeid: str) -> None:
        self._current_test_nodeid = nodeid
        if nodeid not in self._test_records:
            self._test_records[nodeid] = _TestRecord(nodeid=nodeid)
            self._test_order.append(nodeid)
        self._test_start_snapshots[nodeid] = self._snapshot()

    def finish_test(self, nodeid: str) -> None:
        start_snapshot = self._test_start_snapshots.pop(nodeid, None)
        end_snapshot = self._snapshot()
        if start_snapshot is None:
            self._current_test_nodeid = None
            return

        record = self._test_records[nodeid]
        delta = self._diff_snapshots(start_snapshot, end_snapshot)
        record.wall_ms = delta.wall_ms
        record.python_cpu_user_ms = delta.python_cpu_user_ms
        record.python_cpu_sys_ms = delta.python_cpu_sys_ms
        record.cpu_ms_by_bucket = dict(delta.cpu_ms_by_bucket)
        self._current_test_nodeid = None

    def record_test_outcome(self, report: pytest.TestReport) -> None:
        record = self._test_records.get(report.nodeid)
        if record is None:
            return
        if report.when == "call":
            record.outcome = report.outcome
            return
        if report.failed:
            record.outcome = "failed"
            return
        if report.skipped and record.outcome == "unknown":
            record.outcome = "skipped"

    def record_capture_post(self, payload: object) -> None:
        if not isinstance(payload, dict):
            return
        job_id = payload.get("uuid")
        if not isinstance(job_id, str) or not job_id:
            return
        owner = self._current_test_nodeid
        if owner is None:
            return
        self._job_owner_by_id[job_id] = owner

    def record_capture_status(self, job_id: str, payload: object) -> None:
        if not isinstance(payload, dict):
            return
        owner = self._job_owner_by_id.get(job_id)
        if owner is None:
            return
        record = self._test_records.get(owner)
        if record is None or job_id in record._counted_capture_job_ids:
            return
        status = payload.get("status")
        if status not in {"succeeded", "failed"}:
            return

        started_at = payload.get("started_at")
        finished_at = payload.get("finished_at")
        if not isinstance(started_at, str) or not isinstance(finished_at, str):
            return
        started_at_ms = _parse_iso8601_ms(started_at)
        finished_at_ms = _parse_iso8601_ms(finished_at)
        if started_at_ms is None or finished_at_ms is None:
            self.record_error_once(
                f"bad_job_timestamps_{job_id}",
                f"coarse_profile: failed to parse capture timestamps for job {job_id}",
            )
            return
        if finished_at_ms < started_at_ms:
            self.record_error_once(
                f"inverted_job_timestamps_{job_id}",
                f"coarse_profile: capture finished_at < started_at for job {job_id}",
            )
            return

        record.capture_job_wall_ms += finished_at_ms - started_at_ms
        record._counted_capture_job_ids.add(job_id)

    def record_error_once(self, key: str, message: str) -> None:
        if key in self._error_keys:
            return
        self._error_keys.add(key)
        self._errors.append(message)

    def _build_summary_lines(self) -> list[str]:
        if self._session_start_snapshot is None or self._session_end_snapshot is None:
            return []

        session_summary = self._session_summary()
        lines = [f"json: {self.output_path}"]
        python_cpu_ms = session_summary["python_cpu_user_ms"] + session_summary["python_cpu_sys_ms"]
        lines.append(
            "session: "
            f"wall={session_summary['wall_ms']} ms "
            f"python={python_cpu_ms} ms "
            f"webshotd={session_summary['webshotd_cpu_ms']} ms "
            f"chromium={session_summary['chromium_cpu_ms']} ms "
            f"capture_jobs={session_summary['capture_job_wall_ms']} ms"
        )
        services_line = self._format_services_line(session_summary["service_cpu_ms"])
        if services_line is not None:
            lines.append(services_line)

        for title, key in (
            ("top wall", "wall_ms"),
            ("top chromium", "chromium_cpu_ms"),
            ("top python", "_python_cpu_ms"),
        ):
            lines.extend(self._format_top_lines(title=title, key=key))

        if self._errors:
            lines.append("warnings:")
            for message in self._errors:
                lines.append(f"  {message}")

        return lines

    def _build_summary_text(self) -> str:
        lines = self._build_summary_lines()
        return "\n".join(lines) + "\n" if lines else ""

    def _format_top_lines(self, *, title: str, key: str) -> list[str]:
        ranked: list[tuple[int, _TestRecord]] = []
        for nodeid in self._test_order:
            record = self._test_records[nodeid]
            if key == "_python_cpu_ms":
                value = record.python_cpu_user_ms + record.python_cpu_sys_ms
            elif key == "chromium_cpu_ms":
                value = _named_cpu_ms(record.cpu_ms_by_bucket, _BUCKET_CHROMIUM)
            else:
                value = getattr(record, key)
            if value <= 0:
                continue
            ranked.append((value, record))
        ranked.sort(key=lambda item: item[0], reverse=True)
        if not ranked:
            return [f"{title}: none"]

        formatted = [f"{title}:"]
        for value, record in ranked[:5]:
            formatted.append(f"  {value} ms  {record.nodeid} ({record.outcome})")
        return formatted

    @staticmethod
    def _format_services_line(service_cpu_ms: object) -> str | None:
        if not isinstance(service_cpu_ms, dict):
            return None
        items = [
            (str(name), value)
            for name, value in sorted(service_cpu_ms.items())
            if isinstance(value, int) and value > 0
        ]
        if not items:
            return None
        rendered = ", ".join(f"{name}={value} ms" for name, value in items)
        return f"services: {rendered}"

    def _build_json_payload(self) -> dict[str, object]:
        return {
            "meta": {
                "schema_version": _PROFILE_SCHEMA_VERSION,
                "pytest_argv": list(self._config.invocation_params.args),
                "testsuite_working_dir": str(pathlib.Path.cwd()),
                "started_at": self._session_started_at.isoformat(),
                "finished_at": (
                    self._session_finished_at.isoformat()
                    if self._session_finished_at is not None
                    else None
                ),
                "output_path": str(self.output_path),
                "summary_path": str(self.summary_path),
            },
            "profiling_errors": list(self._errors),
            "session": self._session_summary(),
            "tests": [self._test_records[nodeid].to_json() for nodeid in self._test_order],
        }

    def _session_summary(self) -> dict[str, object]:
        if self._session_start_snapshot is None or self._session_end_snapshot is None:
            return {
                "test_count": len(self._test_order),
                "wall_ms": 0,
                "python_cpu_user_ms": 0,
                "python_cpu_sys_ms": 0,
                **_cpu_json_fields({}),
                "capture_job_wall_ms": 0,
            }

        delta = self._diff_snapshots(self._session_start_snapshot, self._session_end_snapshot)
        return {
            "test_count": len(self._test_order),
            "wall_ms": delta.wall_ms,
            "python_cpu_user_ms": delta.python_cpu_user_ms,
            "python_cpu_sys_ms": delta.python_cpu_sys_ms,
            **_cpu_json_fields(delta.cpu_ms_by_bucket),
            "capture_job_wall_ms": sum(
                self._test_records[nodeid].capture_job_wall_ms for nodeid in self._test_order
            ),
        }

    def _snapshot(self) -> _Snapshot:
        proc_table = self._read_proc_table()
        children_by_ppid: dict[int, list[int]] = {}
        for proc in proc_table.values():
            children_by_ppid.setdefault(proc.ppid, []).append(proc.pid)

        self._refresh_s6_service_trackers()
        cpu_ms_by_bucket = {
            bucket: tracker.snapshot_cpu_ms(proc_table, children_by_ppid, self)
            for bucket, tracker in sorted(self._bucket_trackers.items())
        }
        cpu_ms_by_bucket[_BUCKET_CHROMIUM] = self._snapshot_chromium_cpu_ms()

        usage = resource.getrusage(resource.RUSAGE_SELF)
        return _Snapshot(
            wall_ms=time.monotonic_ns() // 1_000_000,
            python_cpu_user_ms=int(usage.ru_utime * 1000),
            python_cpu_sys_ms=int(usage.ru_stime * 1000),
            cpu_ms_by_bucket=cpu_ms_by_bucket,
        )

    def _refresh_s6_service_trackers(self) -> None:
        if not _S6_SCAN_DIR.is_dir():
            return
        try:
            entries = sorted(_S6_SCAN_DIR.iterdir(), key=lambda path: path.name)
        except OSError as exc:
            self.record_error_once(
                "iter_s6_scan_failed",
                f"coarse_profile: failed to read {_S6_SCAN_DIR}: {exc}",
            )
            return

        for entry in entries:
            if not entry.is_dir() or entry.name.startswith(".") or entry.name == "webshotd":
                continue
            bucket = _s6_service_bucket(entry.name)
            if bucket in self._bucket_trackers:
                continue
            status_path = entry / "supervise" / "status"
            self._bucket_trackers[bucket] = _CpuTracker(
                name=bucket,
                pid_resolver=lambda profiler,
                _proc_table,
                *,
                status_path=status_path,
                service_name=entry.name: _resolve_s6_service_pid(
                    profiler=profiler,
                    service_name=service_name,
                    status_path=status_path,
                ),
                include_descendants=True,
            )

    def _snapshot_chromium_cpu_ms(self) -> int:
        webshotd_pid = self._bucket_trackers[_BUCKET_WEBSHOTD].current_pid
        if webshotd_pid is None:
            return self._chromium_cpu_ms
        cgroup_root = self._resolve_cgroup_root_path_from_pid(webshotd_pid)
        if cgroup_root is None:
            return self._chromium_cpu_ms
        try:
            entries = list(cgroup_root.iterdir())
        except OSError as exc:
            self.record_error_once(
                "chromium_cgroup_iter_failed",
                f"coarse_profile: failed to read crawler cgroup root {cgroup_root}: {exc}",
            )
            return self._chromium_cpu_ms

        total_cpu_ms = 0
        found_any = False
        for entry in entries:
            if not entry.is_dir() or not entry.name.startswith("webshotd_crawler_"):
                continue
            cpu_ms = _parse_cgroup_cpu_ms(entry / "cpu.stat")
            if cpu_ms is None:
                continue
            found_any = True
            total_cpu_ms += cpu_ms
        if found_any:
            self._chromium_cpu_ms = max(self._chromium_cpu_ms, total_cpu_ms)
        return self._chromium_cpu_ms

    def _resolve_cgroup_root_path_from_pid(self, pid: int) -> pathlib.Path | None:
        proc_cgroup_path = pathlib.Path("/proc") / str(pid) / "cgroup"
        try:
            lines = proc_cgroup_path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            self.record_error_once(
                "read_proc_cgroup_failed",
                f"coarse_profile: failed to read {proc_cgroup_path}: {exc}",
            )
            return None

        current_path: str | None = None
        for line in lines:
            if not line.startswith("0::"):
                continue
            current_path = line[3:].strip()
            break
        if current_path is None or not current_path.startswith("/"):
            self.record_error_once(
                "missing_proc_cgroup_path",
                f"coarse_profile: missing cgroup v2 path in {proc_cgroup_path}",
            )
            return None
        if current_path == "/":
            self.record_error_once(
                "invalid_proc_cgroup_root",
                "coarse_profile: webshotd is running in '/' cgroup; "
                "crawler cgroups are not attributable",
            )
            return None

        parent_path = pathlib.Path(current_path).parent
        if str(parent_path) == "/":
            return pathlib.Path("/sys/fs/cgroup")
        return pathlib.Path("/sys/fs/cgroup") / str(parent_path).lstrip("/")

    def _read_proc_table(self) -> dict[int, _ProcInfo]:
        proc_table: dict[int, _ProcInfo] = {}
        proc_root = pathlib.Path("/proc")
        for entry in proc_root.iterdir():
            if not entry.name.isdigit():
                continue
            stat_path = entry / "stat"
            cmdline_path = entry / "cmdline"
            try:
                stat_raw = stat_path.read_text(encoding="utf-8")
                cmdline_raw = cmdline_path.read_bytes()
            except OSError:
                continue

            parsed_stat = _parse_proc_stat(stat_raw)
            if parsed_stat is None:
                continue
            pid, ppid, cpu_ms = parsed_stat
            args = tuple(
                part.decode("utf-8", "replace") for part in cmdline_raw.split(b"\0") if part
            )
            proc_table[pid] = _ProcInfo(pid=pid, ppid=ppid, cpu_ms=cpu_ms, args=args)
        return proc_table

    def _resolve_webshotd_pid(
        self,
        profiler: _CoarseProfiler,
        proc_table: dict[int, _ProcInfo],
    ) -> int | None:
        if not self._service_binary_candidates:
            return None
        matches = [
            proc.pid
            for proc in proc_table.values()
            if proc.args and pathlib.Path(proc.args[0]).resolve() in self._service_binary_candidates
        ]
        if len(matches) > 1:
            profiler.record_error_once(
                "multiple_webshotd_pids",
                f"coarse_profile: multiple candidate PIDs for webshotd: {sorted(matches)!r}",
            )
        if not matches:
            return None
        return min(matches)

    @staticmethod
    def _build_service_binary_candidates(
        service_binary: pathlib.Path | None,
    ) -> set[pathlib.Path]:
        if service_binary is None:
            return set()

        candidates = {service_binary}
        sibling_bin = service_binary.parent / "webshotd_bin"
        if sibling_bin.exists():
            candidates.add(sibling_bin.resolve())
        return candidates

    @staticmethod
    def _diff_snapshots(start: _Snapshot, end: _Snapshot) -> _Snapshot:
        bucket_names = set(start.cpu_ms_by_bucket) | set(end.cpu_ms_by_bucket)
        return _Snapshot(
            wall_ms=max(0, end.wall_ms - start.wall_ms),
            python_cpu_user_ms=max(0, end.python_cpu_user_ms - start.python_cpu_user_ms),
            python_cpu_sys_ms=max(0, end.python_cpu_sys_ms - start.python_cpu_sys_ms),
            cpu_ms_by_bucket={
                bucket: max(
                    0, end.cpu_ms_by_bucket.get(bucket, 0) - start.cpu_ms_by_bucket.get(bucket, 0)
                )
                for bucket in sorted(bucket_names)
            },
        )


class _ProfiledServiceClient:
    def __init__(self, client, profiler: _CoarseProfiler) -> None:
        self._client = client
        self._profiler = profiler

    def __getattr__(self, name: str):
        return getattr(self._client, name)

    async def get(self, path: str, **kwargs):
        response = await self._client.get(path, **kwargs)
        self._record_response("GET", path, response)
        return response

    async def post(self, path: str, **kwargs):
        response = await self._client.post(path, **kwargs)
        self._record_response("POST", path, response)
        return response

    def _record_response(self, method: str, path: str, response) -> None:
        try:
            normalized_path = urlsplit(path).path
            if method == "POST" and normalized_path == "/v1/capture" and response.status == 202:
                self._profiler.record_capture_post(response.json())
                return
            if (
                method == "GET"
                and normalized_path.startswith("/v1/capture/jobs/")
                and response.status == 200
            ):
                job_id = normalized_path.rsplit("/", 1)[-1]
                if job_id:
                    self._profiler.record_capture_status(job_id, response.json())
        except Exception as exc:
            self._profiler.record_error_once(
                f"response_parse_failed_{method}_{path}",
                f"coarse_profile: failed to inspect {method} {path}: {exc}",
            )


def _require_profiler(config: pytest.Config) -> _CoarseProfiler:
    profiler = getattr(config, "_webshot_coarse_profiler", None)
    if profiler is None:
        raise RuntimeError("missing _webshot_coarse_profiler on pytest config")
    return profiler


def pytest_configure(config: pytest.Config) -> None:
    config._webshot_coarse_profiler = _CoarseProfiler(config)


def pytest_sessionstart(session: pytest.Session) -> None:
    _require_profiler(session.config).start_session()


def pytest_sessionfinish(session: pytest.Session, exitstatus: int) -> None:
    _require_profiler(session.config).finish_session()


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_protocol(item: pytest.Item, nextitem):
    profiler = _require_profiler(item.config)
    profiler.start_test(item.nodeid)
    yield
    profiler.finish_test(item.nodeid)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item: pytest.Item, call: pytest.CallInfo[None]):
    outcome = yield
    report = outcome.get_result()
    _require_profiler(item.config).record_test_outcome(report)


def pytest_terminal_summary(terminalreporter: pytest.TerminalReporter) -> None:
    del terminalreporter


@pytest.fixture
async def service_client(
    request: pytest.FixtureRequest,
    service_daemon_instance,
    service_baseurl,
    service_client_options,
    userver_service_client_options,
    userver_client_cleanup,
    _testsuite_client_config: userver_client.TestsuiteClientConfig,
):
    profiler = _require_profiler(request.config)
    if not _testsuite_client_config.testsuite_action_path:
        yield _ProfiledServiceClient(
            service_client_plugin._ClientDiagnose(service_baseurl, **service_client_options),
            profiler,
        )
        return

    aiohttp_client = userver_client.AiohttpClient(
        service_baseurl,
        **userver_service_client_options,
    )
    profiled_client = userver_client.Client(aiohttp_client)
    async with userver_client_cleanup(profiled_client):
        yield _ProfiledServiceClient(profiled_client, profiler)
