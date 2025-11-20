from pathlib import Path
import re

import pytest


def _adapt_positional_to_psycopg(sql_text: str) -> str:
    """Convert $1, $2 ... placeholders to %s for psycopg2."""
    return re.sub(r"\$\d+", "%s", sql_text)


@pytest.fixture(scope="session")
def insert_webshot_sql(service_source_dir: Path) -> str:
    path = service_source_dir / "sql" / "queries" / "insert_webshot.sql"
    raw = path.read_text()
    return _adapt_positional_to_psycopg(raw)

