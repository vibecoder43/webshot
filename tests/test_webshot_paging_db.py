import pathlib
import uuid

from helpers.constants import TEST_HOST
from helpers.prefix import prefix_key_from_link, prefix_tree_from_prefix_key
from helpers.sql_loader import _adapt_positional_to_psycopg

_SQL_QUERIES_DIR = pathlib.Path(__file__).resolve().parents[1] / "sql" / "query"
INSERT_WEBSHOT_SQL = _adapt_positional_to_psycopg(
    (_SQL_QUERIES_DIR / "insert_webshot.sql").read_text()
)


async def test_list_webshots_orders_by_created_at(
    service_client,
    pgsql,
):
    """Insert rows via pgsql and verify ordering for /v1/webshot."""

    db = pgsql["capture_meta_db"]

    newer_id = uuid.uuid4()
    older_id = uuid.uuid4()

    cur = db.cursor()
    try:
        prefix_key = prefix_key_from_link(f"{TEST_HOST}/a")
        prefix_tree = prefix_tree_from_prefix_key(prefix_key)
        cur.execute(
            INSERT_WEBSHOT_SQL,
            (
                newer_id,
                f"{TEST_HOST}/a",
                prefix_key,
                prefix_tree,
                f"http://{TEST_HOST}/{newer_id}",
            ),
        )
        cur.execute(
            INSERT_WEBSHOT_SQL,
            (
                older_id,
                f"{TEST_HOST}/a",
                prefix_key,
                prefix_tree,
                f"http://{TEST_HOST}/{older_id}",
            ),
        )
    finally:
        cur.close()

    response = await service_client.get(
        "/v1/webshot",
        params={"link": f"{TEST_HOST}/a"},
    )

    assert response.status == 200
    body = response.json()
    uuids = [item["uuid"] for item in body["items"]]
    assert set(uuids) == {str(newer_id), str(older_id)}


async def test_list_webshots_prefix_sees_inserted_links(
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
            INSERT_WEBSHOT_SQL,
            (
                uuid.uuid4(),
                f"{TEST_HOST}/prefix/a",
                prefix_key_a,
                prefix_tree_a,
                f"http://{TEST_HOST}/prefix/a",
            ),
        )
        prefix_key_b = prefix_key_from_link(f"{TEST_HOST}/prefix/b")
        prefix_tree_b = prefix_tree_from_prefix_key(prefix_key_b)
        cur.execute(
            INSERT_WEBSHOT_SQL,
            (
                uuid.uuid4(),
                f"{TEST_HOST}/prefix/b",
                prefix_key_b,
                prefix_tree_b,
                f"http://{TEST_HOST}/prefix/b",
            ),
        )
    finally:
        cur.close()

    response = await service_client.get(
        "/v1/webshot/prefix",
        params={"prefix": f"{TEST_HOST}/prefix"},
    )

    assert response.status == 200
    body = response.json()
    links = {item["link"] for item in body["items"]}
    assert {f"{TEST_HOST}/prefix/a", f"{TEST_HOST}/prefix/b"}.issubset(links)


async def test_list_webshots_paged_two_pages(
    service_client,
    pgsql,
):
    """Verify /v1/webshot uses page_token to paginate link results."""

    db = pgsql["capture_meta_db"]

    ids = [uuid.uuid4(), uuid.uuid4(), uuid.uuid4()]

    cur = db.cursor()
    try:
        prefix_key = prefix_key_from_link(f"{TEST_HOST}/a")
        prefix_tree = prefix_tree_from_prefix_key(prefix_key)
        # Three rows for the same link; created_at uses default now().
        for webshot_id in ids:
            cur.execute(
                INSERT_WEBSHOT_SQL,
                (
                    webshot_id,
                    f"{TEST_HOST}/a",
                    prefix_key,
                    prefix_tree,
                    f"http://{TEST_HOST}/{webshot_id}",
                ),
            )
    finally:
        cur.close()

    # First page: 2 items (webshots_page_max), next_page_token present.
    resp1 = await service_client.get(
        "/v1/webshot",
        params={"link": f"{TEST_HOST}/a"},
    )
    assert resp1.status == 200
    body1 = resp1.json()
    uuids1 = [item["uuid"] for item in body1["items"]]
    assert len(uuids1) == 2
    next_token = body1.get("next_page_token")
    assert next_token

    # Second page: remaining 1 item, next_page_token is null/absent.
    resp2 = await service_client.get(
        "/v1/webshot",
        params={"link": f"{TEST_HOST}/a", "page_token": next_token},
    )
    assert resp2.status == 200
    body2 = resp2.json()
    uuids2 = [item["uuid"] for item in body2["items"]]
    assert len(uuids2) == 1
    assert set(uuids1 + uuids2) == {str(webshot_id) for webshot_id in ids}
    assert body2.get("next_page_token") in (None, "")
