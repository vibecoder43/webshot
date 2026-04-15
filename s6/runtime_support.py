from __future__ import annotations

import shlex
from pathlib import Path
from typing import NoReturn

from s6.common import die

PROGRAM_NAME = "s6.runtime"


def shell_quote(value: str | Path) -> str:
    return shlex.quote(str(value))


def shell_join(parts: list[str | Path]) -> str:
    return " ".join(shell_quote(part) for part in parts)


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def write_executable(path: Path, content: str) -> None:
    write_text(path, content)
    path.chmod(0o755)


def finish_script() -> str:
    return """#!/bin/sh
if [ "$1" -ne 0 ] && [ "$2" -ne 15 ]; then
  exit 125
fi
exit 0
"""


def wait_script(cmd: list[str | Path]) -> str:
    return (
        f"while ! {shell_quote(cmd[0])}"
        + "".join(f" {shell_quote(part)}" for part in cmd[1:])
        + " >/dev/null 2>&1; do sleep 0.2; done\n"
    )


def format_runtime_message(message: str) -> str:
    prefix = f"{PROGRAM_NAME}: "
    if message.startswith(prefix):
        return message
    return prefix + message


def report(message: str) -> None:
    print(format_runtime_message(message))


def runtime_die(message: str, *, exit_code: int = 2) -> NoReturn:
    die(format_runtime_message(message), exit_code=exit_code)
