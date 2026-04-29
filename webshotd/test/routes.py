from helper.constants import TEST_HOST

INVALID_PAGE_TOKEN_MSG = "page_token: invalid page_token"


async def test_list_captures_missing_link(service_client):
    response = await service_client.get("/v1/capture")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "link: missing parameter"


async def test_list_captures_invalid_page_token(service_client):
    response = await service_client.get(
        "/v1/capture",
        params={"link": f"{TEST_HOST}/a", "page_token": "not-a-token"},
    )

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == INVALID_PAGE_TOKEN_MSG


async def test_list_captures_empty_result(service_client):
    response = await service_client.get(
        "/v1/capture",
        params={"link": f"{TEST_HOST}/a"},
    )

    assert response.status == 200
    body = response.json()
    assert body["items"] == []
    assert "next_page_token" not in body


async def test_list_captures_prefix_missing_prefix(service_client):
    response = await service_client.get("/v1/capture/prefix")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "prefix: missing parameter"


async def test_list_captures_prefix_invalid_page_token(service_client):
    response = await service_client.get(
        "/v1/capture/prefix",
        params={"prefix": f"{TEST_HOST}/a", "page_token": "not-a-token"},
    )

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == INVALID_PAGE_TOKEN_MSG


async def test_list_captures_prefix_empty_result(service_client):
    response = await service_client.get(
        "/v1/capture/prefix",
        params={"prefix": f"{TEST_HOST}/a"},
    )

    assert response.status == 200
    body = response.json()
    assert body["items"] == []
    assert "next_page_token" not in body


async def test_disallow_and_purge_not_exposed_on_main_listener(service_client):
    response = await service_client.post("/v1/denylist/disallow_and_purge")

    assert response.status == 404


async def test_disallow_and_purge_missing_body(monitor_client):
    response = await monitor_client.post("/v1/denylist/disallow_and_purge")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "invalid request body"


async def test_disallow_and_purge_invalid_link(monitor_client):
    # IP literals are rejected
    response = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        json={"link": "127.0.0.1"},
    )

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "invalid parameter"


async def test_create_capture_missing_body(service_client):
    response = await service_client.post("/v1/capture")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "invalid request body"


async def test_create_capture_job_strips_non_default_port_in_normalized_link(service_client):
    response = await service_client.post(
        "/v1/capture",
        json={"link": f"https://{TEST_HOST}:444/path?a=1"},
    )

    assert response.status == 202
    body = response.json()
    assert body["link"] == f"{TEST_HOST}/path?a=1"


async def test_create_capture_denylisted_host(service_client, monitor_client):
    # Insert host into denylist via dedicated endpoint.
    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        json={"link": f"https://{TEST_HOST}/"},
    )
    assert deny_resp.status == 202

    response = await service_client.post(
        "/v1/capture",
        json={"link": f"https://{TEST_HOST}/"},
    )

    assert response.status == 403
    body = response.json()
    assert body["error"]["message"] == "host in denylist"


async def test_create_capture_denylisted_path_blocks_subpaths(service_client, monitor_client):
    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        json={"link": f"https://{TEST_HOST}/a"},
    )
    assert deny_resp.status == 202

    response = await service_client.post(
        "/v1/capture",
        json={"link": f"https://{TEST_HOST}/a/b"},
    )

    assert response.status == 403
    body = response.json()
    assert body["error"]["message"] == "host in denylist"


async def test_create_capture_denylisted_path_does_not_block_sibling_path(
    service_client, monitor_client
):
    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        json={"link": f"https://{TEST_HOST}/a"},
    )
    assert deny_resp.status == 202

    response = await service_client.post(
        "/v1/capture",
        json={"link": f"https://{TEST_HOST}/ab"},
    )

    assert response.status == 202
