import pytest
from helper.constants import TEST_ASSET_HOST, TEST_HOST


@pytest.mark.asyncio
async def test_denylist_check_not_exposed_on_main_listener(service_client):
    resp = await service_client.post("/v1/denylist/check", json={"link": "http://example.com/"})
    assert resp.status == 404


@pytest.mark.asyncio
async def test_denylist_check_requires_post(monitor_client):
    resp = await monitor_client.get("/v1/denylist/check")
    assert resp.status == 405


@pytest.mark.asyncio
async def test_denylist_check_main_listener_get_is_not_exposed(service_client):
    resp = await service_client.get("/v1/denylist/check")
    assert resp.status == 404


@pytest.mark.asyncio
async def test_denylist_check_missing_body(monitor_client):
    resp = await monitor_client.post("/v1/denylist/check", data=b"")
    assert resp.status == 400


@pytest.mark.asyncio
async def test_denylist_check_invalid_body(monitor_client):
    resp = await monitor_client.post("/v1/denylist/check", data=b"not a url")
    assert resp.status == 400
    assert resp.json()["error"]["message"] == "invalid request body"


@pytest.mark.asyncio
async def test_denylist_check_allowed(monitor_client):
    resp = await monitor_client.post(
        "/v1/denylist/check",
        json={"link": f"http://{TEST_HOST}/"},
    )
    assert resp.status == 204


@pytest.mark.asyncio
async def test_denylist_check_denied_after_disallow_and_purge(monitor_client):
    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        json={"link": f"http://{TEST_ASSET_HOST}/"},
    )
    assert deny_resp.status == 202

    resp = await monitor_client.post(
        "/v1/denylist/check",
        json={"link": f"http://{TEST_ASSET_HOST}:123/pixel"},
    )
    assert resp.status == 403


@pytest.mark.asyncio
async def test_denylist_check_ignores_query_when_matching_prefix(monitor_client):
    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        json={"link": f"http://{TEST_ASSET_HOST}/pixel?cache=bust"},
    )
    assert deny_resp.status == 202

    resp = await monitor_client.post(
        "/v1/denylist/check",
        json={"link": f"http://{TEST_ASSET_HOST}/pixel?other=value"},
    )
    assert resp.status == 403
