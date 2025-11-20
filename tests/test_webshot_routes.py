import uuid


async def test_list_webshots_missing_link(service_client):
    response = await service_client.get("/v1/webshot")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "missing parameter: link"


async def test_list_webshots_invalid_page_token(service_client):
    response = await service_client.get(
        "/v1/webshot",
        params={"link": "example.com/a", "page_token": "not-a-token"},
    )

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "invalid page_token"


async def test_list_webshots_empty_result(service_client):
    response = await service_client.get(
        "/v1/webshot",
        params={"link": "example.com/a"},
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
    assert body["error"]["message"] == "missing parameter: prefix"


async def test_list_webshots_prefix_invalid_page_token(service_client):
    response = await service_client.get(
        "/v1/webshot/prefix",
        params={"prefix": "example.com/a", "page_token": "not-a-token"},
    )

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "invalid page_token"


async def test_list_webshots_prefix_empty_result(service_client):
    response = await service_client.get(
        "/v1/webshot/prefix",
        params={"prefix": "example.com/a"},
    )

    assert response.status == 200
    body = response.json()
    assert body["items"] == []
    assert body.get("next_page_token") in (None, "")


async def test_disallow_and_purge_missing_host(service_client):
    response = await service_client.post("/v1/disallow-and-purge")

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "missing parameter: host"


async def test_disallow_and_purge_invalid_host(service_client):
    # IP literals are rejected
    response = await service_client.post(
        "/v1/disallow-and-purge",
        params={"host": "127.0.0.1"},
    )

    assert response.status == 400
    body = response.json()
    assert body["error"]["message"] == "invalid host"

