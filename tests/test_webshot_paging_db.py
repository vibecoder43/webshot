import uuid


async def test_list_webshots_orders_by_created_at(
    service_client,
    pgsql,
    insert_webshot_sql,
):
    """Insert rows via pgsql and verify ordering for /v1/webshot."""

    db = pgsql["capture_meta_db_schema"]
    host = "example.com"
    host_rev = host[::-1]

    newer_id = uuid.uuid4()
    older_id = uuid.uuid4()

    cur = db.cursor()
    try:
        cur.execute(
            insert_webshot_sql,
            (newer_id, "example.com/a", host_rev, f"http://example.com/{newer_id}"),
        )
        cur.execute(
            insert_webshot_sql,
            (older_id, "example.com/a", host_rev, f"http://example.com/{older_id}"),
        )
    finally:
        cur.close()

    response = await service_client.get(
        "/v1/webshot",
        params={"link": "example.com/a"},
    )

    assert response.status == 200
    body = response.json()
    uuids = [item["uuid"] for item in body["items"]]
    assert set(uuids) == {str(newer_id), str(older_id)}


async def test_list_webshots_prefix_sees_inserted_links(
    service_client,
    pgsql,
    insert_webshot_sql,
):
    """Insert two links sharing a prefix and list by prefix."""

    db = pgsql["capture_meta_db_schema"]
    host = "example.com"
    host_rev = host[::-1]

    cur = db.cursor()
    try:
        cur.execute(
            insert_webshot_sql,
            (
                uuid.uuid4(),
                "example.com/prefix/a",
                host_rev,
                "http://example.com/prefix/a",
            ),
        )
        cur.execute(
            insert_webshot_sql,
            (
                uuid.uuid4(),
                "example.com/prefix/b",
                host_rev,
                "http://example.com/prefix/b",
            ),
        )
    finally:
        cur.close()

    response = await service_client.get(
        "/v1/webshot/prefix",
        params={"prefix": "example.com/prefix"},
    )

    assert response.status == 200
    body = response.json()
    links = {item["link"] for item in body["items"]}
    assert {"example.com/prefix/a", "example.com/prefix/b"}.issubset(links)


async def test_list_webshots_paged_two_pages(
    service_client,
    pgsql,
    insert_webshot_sql,
):
    """Verify /v1/webshot uses page_token to paginate link results."""

    db = pgsql["capture_meta_db_schema"]
    host = "example.com"
    host_rev = host[::-1]

    ids = [uuid.uuid4(), uuid.uuid4(), uuid.uuid4()]

    cur = db.cursor()
    try:
        # Three rows for the same link; created_at uses default now().
        for webshot_id in ids:
            cur.execute(
                insert_webshot_sql,
                (
                    webshot_id,
                    "example.com/a",
                    host_rev,
                    f"http://example.com/{webshot_id}",
                ),
            )
    finally:
        cur.close()

    # First page: 2 items (webshots-page-max), next_page_token present.
    resp1 = await service_client.get(
        "/v1/webshot",
        params={"link": "example.com/a"},
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
        params={"link": "example.com/a", "page_token": next_token},
    )
    assert resp2.status == 200
    body2 = resp2.json()
    uuids2 = [item["uuid"] for item in body2["items"]]
    assert len(uuids2) == 1
    assert set(uuids1 + uuids2) == {str(webshot_id) for webshot_id in ids}
    assert body2.get("next_page_token") in (None, "")
