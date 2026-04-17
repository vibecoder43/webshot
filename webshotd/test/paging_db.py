import pathlib
import uuid

from helper.constants import TEST_HOST
from helper.prefix import prefix_key_from_link, prefix_tree_from_prefix_key
from helper.sql_loader import _adapt_positional_to_psycopg

_SQL_QUERIES_DIR = pathlib.Path(__file__).resolve().parents[1] / "sql" / "query"
INSERT_CAPTURE_SQL = _adapt_positional_to_psycopg(
    (_SQL_QUERIES_DIR / "insert_capture.sql").read_text()
)
DUMMY_SHA256 = b"\x00" * 32


async def test_list_captures_orders_by_created_at(
    service_client,
    pgsql,
):
    """Insert rows via pgsql and verify ordering for /v1/capture."""

    db = pgsql["capture_meta_db"]

    newer_id = uuid.uuid4()
    older_id = uuid.uuid4()

    cur = db.cursor()
    try:
        prefix_key = prefix_key_from_link(f"{TEST_HOST}/a")
        prefix_tree = prefix_tree_from_prefix_key(prefix_key)
        cur.execute(
            INSERT_CAPTURE_SQL,
            (
                newer_id,
                f"{TEST_HOST}/a",
                prefix_key,
                prefix_tree,
                DUMMY_SHA256,
            ),
        )
        cur.execute(
            INSERT_CAPTURE_SQL,
            (
                older_id,
                f"{TEST_HOST}/a",
                prefix_key,
                prefix_tree,
                DUMMY_SHA256,
            ),
        )
    finally:
        cur.close()

    response = await service_client.get(
        "/v1/capture",
        params={"link": f"{TEST_HOST}/a"},
    )

    assert response.status == 200
    body = response.json()
    uuids = [item["uuid"] for item in body["items"]]
    assert set(uuids) == {str(newer_id), str(older_id)}


async def test_list_captures_prefix_sees_inserted_links(
    service_client,
    pgsql,
):
    """Insert two links sharing a prefix and list by prefix."""

    db = pgsql["capture_meta_db"]

    cur = db.cursor()
    try:
        prefix_key_a = prefix_key_from_link(f"{TEST_HOST}/prefix/a")
        prefix_tree_a = prefix_tree_from_prefix_key(prefix_key_a)
        cur.execute(
            INSERT_CAPTURE_SQL,
            (
                uuid.uuid4(),
                f"{TEST_HOST}/prefix/a",
                prefix_key_a,
                prefix_tree_a,
                DUMMY_SHA256,
            ),
        )
        prefix_key_b = prefix_key_from_link(f"{TEST_HOST}/prefix/b")
        prefix_tree_b = prefix_tree_from_prefix_key(prefix_key_b)
        cur.execute(
            INSERT_CAPTURE_SQL,
            (
                uuid.uuid4(),
                f"{TEST_HOST}/prefix/b",
                prefix_key_b,
                prefix_tree_b,
                DUMMY_SHA256,
            ),
        )
    finally:
        cur.close()

    response = await service_client.get(
        "/v1/capture/prefix",
        params={"prefix": f"{TEST_HOST}/prefix"},
    )

    assert response.status == 200
    body = response.json()
    links = {item["link"] for item in body["items"]}
    assert {f"{TEST_HOST}/prefix/a", f"{TEST_HOST}/prefix/b"}.issubset(links)


async def test_list_captures_paged_two_pages(
    service_client,
    pgsql,
):
    """Verify /v1/capture uses page_token to paginate link results."""

    db = pgsql["capture_meta_db"]

    ids = [uuid.uuid4(), uuid.uuid4(), uuid.uuid4()]

    cur = db.cursor()
    try:
        prefix_key = prefix_key_from_link(f"{TEST_HOST}/a")
        prefix_tree = prefix_tree_from_prefix_key(prefix_key)
        # Three rows for the same link; created_at uses default now().
        for capture_id in ids:
            cur.execute(
                INSERT_CAPTURE_SQL,
                (
                    capture_id,
                    f"{TEST_HOST}/a",
                    prefix_key,
                    prefix_tree,
                    DUMMY_SHA256,
                ),
            )
    finally:
        cur.close()

    # First page: 2 items (page size), next_page_token present.
    resp1 = await service_client.get(
        "/v1/capture",
        params={"link": f"{TEST_HOST}/a"},
    )
    assert resp1.status == 200
    body1 = resp1.json()
    uuids1 = [item["uuid"] for item in body1["items"]]
    assert len(uuids1) == 2
    next_token = body1.get("next_page_token")
    assert next_token

    # Second page: remaining 1 item, next_page_token is omitted.
    resp2 = await service_client.get(
        "/v1/capture",
        params={"link": f"{TEST_HOST}/a", "page_token": next_token},
    )
    assert resp2.status == 200
    body2 = resp2.json()
    uuids2 = [item["uuid"] for item in body2["items"]]
    assert len(uuids2) == 1
    assert set(uuids1 + uuids2) == {str(capture_id) for capture_id in ids}
    assert "next_page_token" not in body2
