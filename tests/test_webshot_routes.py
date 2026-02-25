from helpers.constants import TEST_HOST

INVALID_PAGE_TOKEN_MSG = "page_token: invalid page_token"


async def test_list_webshots_missing_link(service_client):
    response = await service_client.get("/v1/webshot")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "link: missing parameter"


async def test_list_webshots_invalid_page_token(service_client):
    response = await service_client.get(
        "/v1/webshot",
        params={"link": f"{TEST_HOST}/a", "page_token": "not-a-token"},
    )

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == INVALID_PAGE_TOKEN_MSG


async def test_list_webshots_empty_result(service_client):
    response = await service_client.get(
        "/v1/webshot",
        params={"link": f"{TEST_HOST}/a"},
    )

    assert response.status == 200
    body = response.json()
    assert body["items"] == []
    # next_page_token may be omitted when there is no next page
    assert body.get("next_page_token") in (None, "")


async def test_list_webshots_prefix_missing_prefix(service_client):
    response = await service_client.get("/v1/webshot/prefix")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "prefix: missing parameter"


async def test_list_webshots_prefix_invalid_page_token(service_client):
    response = await service_client.get(
        "/v1/webshot/prefix",
        params={"prefix": f"{TEST_HOST}/a", "page_token": "not-a-token"},
    )

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == INVALID_PAGE_TOKEN_MSG


async def test_list_webshots_prefix_empty_result(service_client):
    response = await service_client.get(
        "/v1/webshot/prefix",
        params={"prefix": f"{TEST_HOST}/a"},
    )

    assert response.status == 200
    body = response.json()
    assert body["items"] == []
    assert body.get("next_page_token") in (None, "")


async def test_disallow_and_purge_missing_host(service_client):
    response = await service_client.post("/v1/disallow_and_purge")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "host: missing parameter"


async def test_disallow_and_purge_invalid_host(service_client):
    # IP literals are rejected
    response = await service_client.post(
        "/v1/disallow_and_purge",
        params={"host": "127.0.0.1"},
    )

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "host: invalid parameter"


async def test_create_webshot_missing_body(service_client):
    response = await service_client.post("/v1/webshot")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "invalid request body"


async def test_create_webshot_denylisted_host(service_client):
    # Insert host into denylist via dedicated endpoint.
    deny_resp = await service_client.post(
        "/v1/disallow_and_purge",
        params={"host": f"https://{TEST_HOST}/"},
    )
    assert deny_resp.status == 202

    response = await service_client.post(
        "/v1/webshot",
        json={"link": f"https://{TEST_HOST}/"},
    )

    assert response.status == 403
    body = response.json()
    assert body["error"]["message"] == "host in denylist"


async def test_create_webshot_denylisted_path_blocks_subpaths(service_client):
    deny_resp = await service_client.post(
        "/v1/disallow_and_purge",
        params={"host": f"https://{TEST_HOST}/a"},
    )
    assert deny_resp.status == 202

    response = await service_client.post(
        "/v1/webshot",
        json={"link": f"https://{TEST_HOST}/a/b"},
    )

    assert response.status == 403
    body = response.json()
    assert body["error"]["message"] == "host in denylist"


async def test_create_webshot_denylisted_path_does_not_block_sibling_path(service_client):
    deny_resp = await service_client.post(
        "/v1/disallow_and_purge",
        params={"host": f"https://{TEST_HOST}/a"},
    )
    assert deny_resp.status == 202

    response = await service_client.post(
        "/v1/webshot",
        json={"link": f"https://{TEST_HOST}/ab"},
    )

    assert response.status == 202
