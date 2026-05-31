import pytest
from helper.config_hooks import enable_allowlist_only
from helper.constants import TEST_ASSET_HOST, TEST_HOST


@pytest.mark.asyncio
async def test_allowlist_check_not_exposed_on_main_listener(service_client):
    resp = await service_client.post("/v1/allowlist/check", json={"link": "http://example.com/"})
    assert resp.status == 404


@pytest.mark.asyncio
async def test_allowlist_add_not_exposed_on_main_listener(service_client):
    resp = await service_client.post("/v1/allowlist/add", json={"link": "example.com"})
    assert resp.status == 404


@pytest.mark.asyncio
async def test_allowlist_remove_not_exposed_on_main_listener(service_client):
    resp = await service_client.post("/v1/allowlist/remove", json={"link": "example.com"})
    assert resp.status == 404


@pytest.mark.asyncio
async def test_allowlist_check_requires_post(monitor_client):
    resp = await monitor_client.get("/v1/allowlist/check")
    assert resp.status == 405


@pytest.mark.asyncio
async def test_allowlist_check_missing_body(monitor_client):
    resp = await monitor_client.post("/v1/allowlist/check", data=b"")
    assert resp.status == 400


@pytest.mark.asyncio
async def test_allowlist_check_invalid_body(monitor_client):
    resp = await monitor_client.post("/v1/allowlist/check", data=b"not a link")
    assert resp.status == 400
    assert resp.json()["error"]["message"] == "invalid request body"


@pytest.mark.asyncio
async def test_allowlist_add_missing_link(monitor_client):
    resp = await monitor_client.post("/v1/allowlist/add")
    assert resp.status == 400
    assert resp.json()["error"]["message"] == "invalid request body"


@pytest.mark.asyncio
async def test_allowlist_add_check_and_remove(monitor_client):
    link = f"http://{TEST_ASSET_HOST}/pixel?cache=bust"

    resp = await monitor_client.post("/v1/allowlist/check", json={"link": link})
    assert resp.status == 403

    add_resp = await monitor_client.post("/v1/allowlist/add", json={"link": link})
    assert add_resp.status == 204

    resp = await monitor_client.post(
        "/v1/allowlist/check",
        json={"link": f"http://{TEST_ASSET_HOST}/pixel?other=value"},
    )
    assert resp.status == 204

    remove_resp = await monitor_client.post("/v1/allowlist/remove", json={"link": link})
    assert remove_resp.status == 204

    resp = await monitor_client.post("/v1/allowlist/check", json={"link": link})
    assert resp.status == 403


@pytest.mark.asyncio
async def test_regular_mode_allowlist_overrides_denylist(service_client, monitor_client):
    link = f"https://{TEST_HOST}/allowlist-overrides-denylist"

    allow_resp = await monitor_client.post("/v1/allowlist/add", json={"link": link})
    assert allow_resp.status == 204

    deny_resp = await monitor_client.post("/v1/denylist/deny-and-purge", json={"link": link})
    assert deny_resp.status == 202

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202


@pytest.mark.uservice_oneshot(config_hooks=[enable_allowlist_only])
@pytest.mark.asyncio
async def test_allowlist_only_blocks_non_allowlisted_seed(service_client):
    resp = await service_client.post(
        "/v1/capture",
        json={"link": f"https://{TEST_HOST}/allowlist-only-blocked"},
    )
    assert resp.status == 403
    assert resp.json()["error"]["message"] == "link not in allowlist"


@pytest.mark.uservice_oneshot(config_hooks=[enable_allowlist_only])
@pytest.mark.asyncio
async def test_allowlist_only_denylist_wins(service_client, monitor_client):
    link = f"https://{TEST_HOST}/allowlist-only-denylist-wins"

    allow_resp = await monitor_client.post("/v1/allowlist/add", json={"link": link})
    assert allow_resp.status == 204

    deny_resp = await monitor_client.post("/v1/denylist/deny-and-purge", json={"link": link})
    assert deny_resp.status == 202

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 403
    assert resp.json()["error"]["message"] == "link prefix in denylist"
