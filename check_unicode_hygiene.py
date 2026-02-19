#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import unicodedata
from dataclasses import dataclass

ASCII_ONLY_EXTENSIONS = {
    ".c",
    ".cc",
    ".cmake",
    ".cpp",
    ".cu",
    ".cxx",
    ".h",
    ".hpp",
    ".hxx",
    ".in",
    ".ini",
    ".json",
    ".nix",
    ".proto",
    ".py",
    ".sql",
    ".toml",
    ".txt",
    ".yaml",
    ".yml",
}

UNICODE_ALLOWED_EXTENSIONS = {
    ".md",
}

# Disallow "smart" punctuation and whitespace that is commonly introduced accidentally and is hard
# to see in diffs. This list is intentionally small and biased toward real-world breakages.
BANNED_CODEPOINTS = {
    # Curly quotes
    0x2018,  # LEFT SINGLE QUOTATION MARK
    0x2019,  # RIGHT SINGLE QUOTATION MARK
    0x201C,  # LEFT DOUBLE QUOTATION MARK
    0x201D,  # RIGHT DOUBLE QUOTATION MARK
    # Dashes/minus
    0x2010,  # HYPHEN
    0x2011,  # NON-BREAKING HYPHEN
    0x2012,  # FIGURE DASH
    0x2013,  # EN DASH
    0x2014,  # EM DASH
    0x2212,  # MINUS SIGN
    # Ellipsis/bullets (often pasted into prose)
    0x2026,  # HORIZONTAL ELLIPSIS
    0x2022,  # BULLET
    # Common invisible/space-like
    0x00A0,  # NO-BREAK SPACE
    0x202F,  # NARROW NO-BREAK SPACE
    0x200B,  # ZERO WIDTH SPACE
    0x200C,  # ZERO WIDTH NON-JOINER
    0x200D,  # ZERO WIDTH JOINER
    0x2060,  # WORD JOINER
    0xFEFF,  # ZERO WIDTH NO-BREAK SPACE (BOM)
    # Common Latin lookalike homoglyphs (Cyrillic/Greek) that are hard to spot in diffs.
    0x0391,  # GREEK CAPITAL LETTER ALPHA
    0x03B1,  # GREEK SMALL LETTER ALPHA
    0x039F,  # GREEK CAPITAL LETTER OMICRON
    0x03BF,  # GREEK SMALL LETTER OMICRON
    0x0410,  # CYRILLIC CAPITAL LETTER A
    0x0430,  # CYRILLIC SMALL LETTER A
    0x0415,  # CYRILLIC CAPITAL LETTER IE
    0x0435,  # CYRILLIC SMALL LETTER IE
    0x041E,  # CYRILLIC CAPITAL LETTER O
    0x043E,  # CYRILLIC SMALL LETTER O
    0x0420,  # CYRILLIC CAPITAL LETTER ER
    0x0440,  # CYRILLIC SMALL LETTER ER
    0x0421,  # CYRILLIC CAPITAL LETTER ES
    0x0441,  # CYRILLIC SMALL LETTER ES
    0x0425,  # CYRILLIC CAPITAL LETTER HA
    0x0445,  # CYRILLIC SMALL LETTER HA
    0x0406,  # CYRILLIC CAPITAL LETTER BYELORUSSIAN-UKRAINIAN I
    0x0456,  # CYRILLIC SMALL LETTER BYELORUSSIAN-UKRAINIAN I
}


def _git_ls_files() -> list[str]:
    out = subprocess.check_output(["git", "ls-files", "-z"])
    parts = out.split(b"\x00")
    paths: list[str] = []
    for p in parts:
        if not p:
            continue
        paths.append(p.decode("utf-8", errors="strict"))
    return paths


@dataclass(frozen=True)
class Issue:
    path: str
    line: int
    column: int
    codepoint: int
    name: str
    message: str

    def format(self) -> str:
        return (
            f"{self.path}:{self.line}:{self.column}: "
            f"U+{self.codepoint:04X} {self.name}: {self.message}"
        )


def _should_be_ascii_only(path: str) -> bool:
    _, ext = os.path.splitext(path)
    if ext in UNICODE_ALLOWED_EXTENSIONS:
        return False
    if ext in ASCII_ONLY_EXTENSIONS:
        return True
    # Default to ASCII-only for unknown extensions to avoid surprises.
    return True


def _iter_issues_for_text(path: str, text: str, enforce_ascii_only: bool) -> list[Issue]:
    issues: list[Issue] = []
    line = 1
    col = 0
    for ch in text:
        if ch == "\n":
            line += 1
            col = 0
            continue
        col += 1

        cp = ord(ch)

        # Disallow most control chars (keep tab for code; allow CR for Windows line endings).
        if cp < 0x20 and ch not in ("\t", "\r"):
            issues.append(
                Issue(
                    path=path,
                    line=line,
                    column=col,
                    codepoint=cp,
                    name=unicodedata.name(ch, "CONTROL"),
                    message="control character is not allowed",
                )
            )
            continue

        # Always ban known-problematic codepoints.
        if cp in BANNED_CODEPOINTS:
            issues.append(
                Issue(
                    path=path,
                    line=line,
                    column=col,
                    codepoint=cp,
                    name=unicodedata.name(ch, "UNKNOWN"),
                    message="banned unicode character (confusable/invisible)",
                )
            )
            continue

        # Ban Unicode format characters (includes bidi controls and many invisibles).
        cat = unicodedata.category(ch)
        if cat == "Cf":
            issues.append(
                Issue(
                    path=path,
                    line=line,
                    column=col,
                    codepoint=cp,
                    name=unicodedata.name(ch, "UNKNOWN"),
                    message="unicode format character is not allowed",
                )
            )
            continue

        # Ban non-ASCII whitespace (hard to spot in diffs).
        if cat == "Zs" and ch != " ":
            issues.append(
                Issue(
                    path=path,
                    line=line,
                    column=col,
                    codepoint=cp,
                    name=unicodedata.name(ch, "UNKNOWN"),
                    message="non-ASCII whitespace is not allowed (use ASCII space)",
                )
            )
            continue

        # Enforce ASCII-only where required.
        if enforce_ascii_only and cp >= 0x80:
            issues.append(
                Issue(
                    path=path,
                    line=line,
                    column=col,
                    codepoint=cp,
                    name=unicodedata.name(ch, "UNKNOWN"),
                    message="non-ASCII character not allowed in this file type",
                )
            )
            continue

        # Ban unicode compatibility forms that normalize into plain ASCII (often visually
        # indistinguishable in diffs).
        if cp >= 0x80:
            nfkc = unicodedata.normalize("NFKC", ch)
            if nfkc != ch and nfkc and all(ord(x) < 0x80 for x in nfkc):
                issues.append(
                    Issue(
                        path=path,
                        line=line,
                        column=col,
                        codepoint=cp,
                        name=unicodedata.name(ch, "UNKNOWN"),
                        message=f"unicode compatibility confusable; use ASCII {nfkc!r} instead",
                    )
                )
                continue

    return issues


def _read_utf8_text(path: str) -> str | None:
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"{path}: failed to read: {e}", file=sys.stderr)
        return None

    try:
        return data.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        # Skip likely-binary tracked files (images, archives, etc).
        return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Check repo files for unicode hygiene issues.")
    parser.add_argument(
        "paths",
        nargs="*",
        help="Files to check (default: all tracked files via git ls-files).",
    )
    args = parser.parse_args(argv)

    paths = args.paths or _git_ls_files()

    all_issues: list[Issue] = []
    for path in paths:
        text = _read_utf8_text(path)
        if text is None:
            continue
        enforce_ascii_only = _should_be_ascii_only(path)
        all_issues.extend(_iter_issues_for_text(path, text, enforce_ascii_only=enforce_ascii_only))

    if all_issues:
        for issue in all_issues:
            print(issue.format(), file=sys.stderr)
        print(f"unicode hygiene: {len(all_issues)} issue(s) found", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
