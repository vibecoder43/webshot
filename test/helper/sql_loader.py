import re


def _adapt_positional_to_psycopg(sql_text: str) -> str:
    """Convert $1, $2 ... placeholders to %s for psycopg2."""
    out: list[str] = []
    for line in sql_text.splitlines(keepends=True):
        idx = line.find("--")
        if idx == -1:
            out.append(re.sub(r"\$\d+", "%s", line))
            continue
        out.append(re.sub(r"\$\d+", "%s", line[:idx]))
        out.append(line[idx:])
    return "".join(out)
