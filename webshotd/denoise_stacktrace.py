#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent
_HEADER_PREFIX = "ERROR at "
_STACKTRACE_SUFFIX = ". Stacktrace:"
_FRAME_RE = re.compile(r"^\s*(?P<index>\d+)#\s+(?P<body>.+)$")
_RUNTIME_BINARY_PLACEHOLDER = "<binary>"
_NIX_STORE_PREFIX = Path("/nix/store")

_PROGRAM_SYMBOL_MARKERS = (
    "v1::",
    "dto::",
    "CdpClient::",
    "PageTracker::",
    "CaptureSession::",
    "Crud::",
    "CrawlerRunner::",
    "executeRun(",
)
_ASSERT_SYMBOL_MARKERS = (
    "AbortWithStacktrace",
    "UASSERT_failed",
)
_BOUNDARY_SYMBOL_MARKERS = (
    "WrappedCallImpl",
    "TaskContext::CoroFunc",
)
_WRAPPER_SYMBOL_MARKERS = (
    "boost::coroutines2::",
    "boost::context::",
    "std::__invoke",
    "std::invoke",
    "fiber_capture_record",
    "fiber_entry_func",
    "__start_context",
    "[start of coroutine]",
)


@dataclass(frozen=True)
class Frame:
    index: int
    symbol: str
    location: str | None
    source: str | None


@dataclass(frozen=True)
class TraceBlock:
    header: str
    frames: tuple[Frame, ...]


_STRING_REWRITES: tuple[tuple[re.Pattern[str], str], ...] = (
    (
        re.compile(
            r"std::__cxx11::basic_string<char,\s*std::char_traits<char>,\s*"
            r"std::allocator<char>\s*>"
        ),
        "std::string",
    ),
    (
        re.compile(r"std::basic_string_view<char,\s*std::char_traits<char>\s*>"),
        "std::string_view",
    ),
    (
        re.compile(
            r"boost::safe_numerics::safe_base<[^,]+,\s*-9223372036854775808l,\s*"
            r"9223372036854775807l,\s*boost::safe_numerics::native,\s*"
            r"boost::safe_numerics::exception_policy<integers::SafeIntegerAbort,\s*"
            r"integers::SafeIntegerAbort,\s*integers::SafeIntegerAbort,\s*"
            r"integers::TrapUninitialized>\s*>"
        ),
        "i64",
    ),
    (
        re.compile(
            r"boost::safe_numerics::safe_base<[^,]+,\s*0[a-z]*,\s*18446744073709551615[a-z]*,\s*"
            r"boost::safe_numerics::native,\s*"
            r"boost::safe_numerics::exception_policy<integers::SafeIntegerAbort,\s*"
            r"integers::SafeIntegerAbort,\s*integers::SafeIntegerAbort,\s*"
            r"integers::TrapUninitialized>\s*>"
        ),
        "u64",
    ),
    (
        re.compile(
            r"boost::safe_numerics::safe_base<[^,]+,\s*-2147483648,\s*2147483647,\s*"
            r"boost::safe_numerics::native,\s*"
            r"boost::safe_numerics::exception_policy<integers::SafeIntegerAbort,\s*"
            r"integers::SafeIntegerAbort,\s*integers::SafeIntegerAbort,\s*"
            r"integers::TrapUninitialized>\s*>"
        ),
        "i32",
    ),
    (
        re.compile(
            r"boost::safe_numerics::safe_base<[^,]+,\s*0[a-z]*,\s*4294967295[a-z]*,\s*"
            r"boost::safe_numerics::native,\s*"
            r"boost::safe_numerics::exception_policy<integers::SafeIntegerAbort,\s*"
            r"integers::SafeIntegerAbort,\s*integers::SafeIntegerAbort,\s*"
            r"integers::TrapUninitialized>\s*>"
        ),
        "u32",
    ),
    (
        re.compile(
            r"boost::safe_numerics::safe_base<[^,]+,\s*0[a-z]*,\s*65535[a-z]*,\s*"
            r"boost::safe_numerics::native,\s*"
            r"boost::safe_numerics::exception_policy<integers::SafeIntegerAbort,\s*"
            r"integers::SafeIntegerAbort,\s*integers::SafeIntegerAbort,\s*"
            r"integers::TrapUninitialized>\s*>"
        ),
        "u16",
    ),
    (re.compile(r"userver::v\d+_\d+(?:_[a-z]+)?::"), "userver::"),
)


def _normalize_signature(text: str) -> str:
    normalized = text
    for pattern, replacement in _STRING_REWRITES:
        normalized = pattern.sub(replacement, normalized)
    normalized = re.sub(r"\s+", " ", normalized)
    normalized = normalized.replace("> >", ">>")
    return normalized.strip()


def _omit_signature_args(symbol: str) -> str:
    candidates: list[tuple[int, int]] = []
    arg_start: int | None = None
    template_depth = 0
    paren_depth = 0

    for index, char in enumerate(symbol):
        if char == "<":
            template_depth += 1
            continue
        if char == ">":
            template_depth = max(0, template_depth - 1)
            continue
        if template_depth > 0:
            continue
        if char == "(":
            if paren_depth == 0:
                arg_start = index
            paren_depth += 1
            continue
        if char != ")" or paren_depth == 0:
            continue
        paren_depth -= 1
        if paren_depth == 0 and arg_start is not None:
            suffix = symbol[index + 1 :].lstrip()
            if not suffix.startswith("::"):
                candidates.append((arg_start, index))
            arg_start = None
            continue

    if not candidates:
        return symbol

    arg_start, arg_end = candidates[-1]
    return f"{symbol[:arg_start]}(){symbol[arg_end + 1 :]}"


def _normalize_path(path: str) -> str:
    stripped = path.strip()
    path_obj = Path(stripped)
    if path_obj.is_absolute():
        if _NIX_STORE_PREFIX in path_obj.parents:
            nix_parts = path_obj.parts
            store_index = nix_parts.index("store")
            if len(nix_parts) > store_index + 2:
                return "/".join(nix_parts[store_index + 2 :])
            return stripped
        if _REPO_ROOT in path_obj.parents and "runtime_root" in path_obj.parts:
            return _RUNTIME_BINARY_PLACEHOLDER
        try:
            return path_obj.relative_to(_REPO_ROOT).as_posix()
        except ValueError:
            return stripped
    return stripped


def _normalize_header(header: str) -> str:
    return header.replace(f"{_REPO_ROOT.as_posix()}/", "")


def _parse_frame(line: str) -> Frame | None:
    match = _FRAME_RE.match(line)
    if not match:
        return None

    body = match.group("body").strip()
    source: str | None = None
    if " at " in body:
        body, _, source = body.rpartition(" at ")

    location: str | None = None
    symbol = body
    if " in " in body:
        symbol, _, location = body.rpartition(" in ")

    return Frame(
        index=int(match.group("index")),
        symbol=symbol.strip(),
        location=location.strip() if location else None,
        source=source.strip() if source else None,
    )


def _extract_blocks(text: str) -> list[TraceBlock]:
    lines = text.splitlines()
    blocks: list[TraceBlock] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        if not (line.startswith(_HEADER_PREFIX) and line.endswith(_STACKTRACE_SUFFIX)):
            index += 1
            continue

        frames: list[Frame] = []
        cursor = index + 1
        while cursor < len(lines):
            frame = _parse_frame(lines[cursor])
            if frame is None:
                break
            frames.append(frame)
            cursor += 1

        if frames:
            blocks.append(TraceBlock(header=line, frames=tuple(frames)))
        index = cursor if cursor > index else index + 1

    return blocks


def _has_repo_source(frame: Frame) -> bool:
    if frame.source is None:
        return False
    source = Path(frame.source)
    return source.is_absolute() and _REPO_ROOT in source.parents


def _has_repo_location(frame: Frame) -> bool:
    if frame.location is None:
        return False
    location = Path(frame.location)
    return location.is_absolute() and _REPO_ROOT in location.parents


def _is_assert_frame(frame: Frame) -> bool:
    return any(marker in frame.symbol for marker in _ASSERT_SYMBOL_MARKERS)


def _is_program_frame(frame: Frame) -> bool:
    if _has_repo_source(frame):
        return True
    if _has_repo_location(frame) and any(
        marker in frame.symbol for marker in _PROGRAM_SYMBOL_MARKERS
    ):
        return True
    return any(marker in frame.symbol for marker in _PROGRAM_SYMBOL_MARKERS)


def _is_wrapper_noise(frame: Frame) -> bool:
    return any(marker in frame.symbol for marker in _WRAPPER_SYMBOL_MARKERS)


def _is_boundary_candidate(frame: Frame) -> bool:
    if _is_wrapper_noise(frame):
        return False
    if any(marker in frame.symbol for marker in _BOUNDARY_SYMBOL_MARKERS):
        return True
    return (
        frame.location is not None
        and _normalize_path(frame.location) == _RUNTIME_BINARY_PLACEHOLDER
    )


def _classify_omission(frames: list[Frame]) -> str:
    if any(_is_wrapper_noise(frame) for frame in frames):
        return "coroutine/wrapper"
    return "framework/runtime"


def _collect_keep_indexes(frames: tuple[Frame, ...]) -> set[int]:
    keep = {
        idx
        for idx, frame in enumerate(frames)
        if _is_assert_frame(frame) or _is_program_frame(frame)
    }
    if not keep:
        return set(range(min(8, len(frames))))

    first = min(keep)
    last = max(keep)

    before_budget = 2
    cursor = first - 1
    while cursor >= 0 and before_budget > 0:
        frame = frames[cursor]
        if _is_boundary_candidate(frame):
            keep.add(cursor)
            before_budget -= 1
            cursor -= 1
            continue
        break

    after_budget = 2
    cursor = last + 1
    while cursor < len(frames) and after_budget > 0:
        frame = frames[cursor]
        if _is_boundary_candidate(frame):
            keep.add(cursor)
            after_budget -= 1
            cursor += 1
            continue
        break

    return keep


def _render_frame(frame: Frame, *, omit_args: bool) -> str:
    symbol = _normalize_signature(frame.symbol)
    if omit_args:
        symbol = _omit_signature_args(symbol)
    if frame.source is not None:
        return f"{frame.index:>2}# {symbol} at {_normalize_path(frame.source)}"
    if frame.location is not None:
        return f"{frame.index:>2}# {symbol} in {_normalize_path(frame.location)}"
    return f"{frame.index:>2}# {symbol}"


def _render_block(block: TraceBlock, *, omit_args: bool) -> str:
    keep_indexes = _collect_keep_indexes(block.frames)
    rendered = [_normalize_header(block.header)]
    omitted: list[Frame] = []

    for idx, frame in enumerate(block.frames):
        if idx in keep_indexes:
            if omitted:
                reason = _classify_omission(omitted)
                rendered.append(f"    [omitted {len(omitted)} {reason} frames]")
                omitted.clear()
            rendered.append(f" {_render_frame(frame, omit_args=omit_args)}")
            continue
        omitted.append(frame)

    if omitted:
        reason = _classify_omission(omitted)
        rendered.append(f"    [omitted {len(omitted)} {reason} frames]")

    return "\n".join(rendered)


def _read_input(path_arg: str | None) -> str:
    if path_arg is None or path_arg == "-":
        return sys.stdin.read()
    return Path(path_arg).read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract and denoise pretty multiline stacktraces."
    )
    parser.add_argument(
        "--omit-args",
        action="store_true",
        help="Strip function argument lists from rendered frame symbols.",
    )
    parser.add_argument("path", nargs="?", help="Log file path, or '-' / omitted for stdin")
    args = parser.parse_args()

    text = _read_input(args.path)
    blocks = _extract_blocks(text)
    if not blocks:
        print("no pretty multiline stacktrace blocks found", file=sys.stderr)
        return 1

    sys.stdout.write(
        "\n\n".join(_render_block(block, omit_args=args.omit_args) for block in blocks)
    )
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
