#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

_IGNORED_SUFFIXES = (
    ".ddi",
    ".modmap",
)
_IGNORED_NAMES = {
    "CXX.dd",
    "CXXModules.json",
}
_COMPILE_SUFFIXES = (
    ".o",
    ".pch",
)


@dataclass(frozen=True)
class NinjaLogRow:
    start_ms: int
    end_ms: int
    mtime: int
    output: str
    command_hash: str

    @property
    def duration_ms(self) -> int:
        return self.end_ms - self.start_ms

    @property
    def key(self) -> tuple[int, int, int, str]:
        return (self.start_ms, self.end_ms, self.mtime, self.command_hash)


@dataclass(frozen=True)
class BuildEdge:
    start_ms: int
    end_ms: int
    command_hash: str
    outputs: tuple[str, ...]

    @property
    def duration_ms(self) -> int:
        return self.end_ms - self.start_ms

    def is_compile(self) -> bool:
        return any(output.endswith(_COMPILE_SUFFIXES) for output in self.outputs)

    def display_output(self) -> str:
        primary = self.outputs[0]
        if len(self.outputs) == 1:
            return primary
        return f"{primary} (+{len(self.outputs) - 1} more)"

    def to_json(self) -> dict[str, object]:
        return {
            "output": self.display_output(),
            "outputs": list(self.outputs),
            "duration_ms": self.duration_ms,
            "kind": "compile" if self.is_compile() else "other",
        }


def _read_log(path: Path) -> list[NinjaLogRow]:
    if not path.exists():
        return []

    rows: list[NinjaLogRow] = []
    with path.open(encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue

            fields = line.split("\t")
            if len(fields) != 5:
                raise RuntimeError(f"unexpected .ninja_log row in {path}: {line!r}")

            start_ms, end_ms, mtime, output, command_hash = fields
            rows.append(
                NinjaLogRow(
                    start_ms=int(start_ms),
                    end_ms=int(end_ms),
                    mtime=int(mtime),
                    output=output,
                    command_hash=command_hash,
                )
            )
    return rows


def _subtract_rows(before: list[NinjaLogRow], after: list[NinjaLogRow]) -> list[NinjaLogRow]:
    remaining = Counter(
        (row.start_ms, row.end_ms, row.mtime, row.output, row.command_hash) for row in before
    )
    new_rows: list[NinjaLogRow] = []
    for row in after:
        identity = (row.start_ms, row.end_ms, row.mtime, row.output, row.command_hash)
        if remaining[identity] > 0:
            remaining[identity] -= 1
            continue
        new_rows.append(row)
    return new_rows


def _should_ignore_output(output: str) -> bool:
    if output.endswith(_IGNORED_SUFFIXES):
        return True
    return Path(output).name in _IGNORED_NAMES


def _normalize_output(output: str, build_dir: Path) -> str:
    output_path = Path(output)
    if output_path.is_absolute():
        try:
            return output_path.relative_to(build_dir).as_posix()
        except ValueError:
            return output
    return output


def _group_edges(rows: list[NinjaLogRow], build_dir: Path) -> list[BuildEdge]:
    grouped: dict[tuple[int, int, int, str], list[str]] = {}
    for row in rows:
        if _should_ignore_output(row.output):
            continue
        grouped.setdefault(row.key, []).append(_normalize_output(row.output, build_dir))

    edges: list[BuildEdge] = []
    for (start_ms, end_ms, _mtime, command_hash), outputs in grouped.items():
        outputs = sorted(set(outputs))
        edges.append(
            BuildEdge(
                start_ms=start_ms,
                end_ms=end_ms,
                command_hash=command_hash,
                outputs=tuple(outputs),
            )
        )
    edges.sort(key=lambda edge: (-edge.duration_ms, edge.display_output()))
    return edges


def _top_edges(edges: list[BuildEdge], limit: int) -> list[dict[str, object]]:
    return [edge.to_json() for edge in edges[:limit]]


def _serialize_edges(edges: list[BuildEdge]) -> list[dict[str, object]]:
    return [edge.to_json() for edge in edges]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--before-log", required=True)
    parser.add_argument("--after-log", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--status", required=True, choices=("success", "failure"))
    parser.add_argument("--started-at", required=True)
    parser.add_argument("--finished-at", required=True)
    parser.add_argument("--wall-time-ms", required=True, type=int)
    parser.add_argument("--top-limit", type=int, default=10)
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    before_rows = _read_log(Path(args.before_log))
    after_rows = _read_log(Path(args.after_log))
    new_rows = _subtract_rows(before_rows, after_rows)
    edges = _group_edges(new_rows, build_dir)

    compile_edges = [edge for edge in edges if edge.is_compile()]
    other_edges = [edge for edge in edges if not edge.is_compile()]

    payload = {
        "status": args.status,
        "build_dir": args.build_dir,
        "started_at": args.started_at,
        "finished_at": args.finished_at,
        "wall_time_ms": args.wall_time_ms,
        "compile_step_count": len(compile_edges),
        "other_step_count": len(other_edges),
        "compile_steps": _serialize_edges(compile_edges),
        "other_steps": _serialize_edges(other_edges),
        "slowest_compile_steps": _top_edges(compile_edges, args.top_limit),
        "slowest_other_steps": _top_edges(other_edges, args.top_limit),
    }

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
