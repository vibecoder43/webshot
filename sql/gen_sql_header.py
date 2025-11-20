#!/usr/bin/env python3

import argparse
import pathlib


def _to_const_name(stem: str) -> str:
    parts = stem.split("_")
    camel = "".join(part.capitalize() for part in parts if part)
    return f"k{camel}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    input_dir = pathlib.Path(args.input_dir)
    output_path = pathlib.Path(args.output)

    sql_files = sorted(input_dir.glob("*.sql"))

    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("#include <string_view>")
    lines.append("")
    lines.append("namespace sql {")
    lines.append("")

    for path in sql_files:
        const_name = _to_const_name(path.stem)
        sql_text = path.read_text()
        if not sql_text.endswith("\n"):
            sql_text += "\n"

        lines.append(f"inline constexpr std::string_view {const_name} = R\"~(")
        lines.append(sql_text.rstrip("\n"))
        lines.append(")~\";")
        lines.append("")

    lines.append("} // namespace sql")
    lines.append("")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines))


if __name__ == "__main__":
    main()

