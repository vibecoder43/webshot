import asyncio

import pytest
from helper.constants import TEST_HOST

_CLIENT_IP_HEADER = "X-Test-Client-IP"
_CLIENT_IP = "198.51.100.9"
_OTHER_CLIENT_IP = "198.51.100.10"
_THIRD_CLIENT_IP = "198.51.100.11"
_CLIENT_IPV6 = "2001:db8::9"
_CLIENT_IPV6_BRACKETED = f"[{_CLIENT_IPV6}]"
_IP_RATELIMIT_MS = 200
_LINK_RATELIMIT_SEC = 1.0


def _enable_ip_ratelimit(_config_yaml, config_vars):
    config_vars["interval_ms"] = _IP_RATELIMIT_MS
    config_vars["link_ratelimit_sec"] = int(_LINK_RATELIMIT_SEC)
    config_vars["client_ip_source"] = "trusted_header"
    config_vars["client_ip_header_name"] = _CLIENT_IP_HEADER


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_ratelimit])
async def test_create_capture_respects_link_ratelimit(service_client):
    link = f"https://{TEST_HOST}/ratelimit-path"

    resp1 = await service_client.post(
        "/v1/capture", json={"link": link}, headers={_CLIENT_IP_HEADER: _CLIENT_IP}
    )
    assert resp1.status == 202
    job1 = resp1.json()

    # A different client IP is not blocked by IP ratelimit, so link ratelimit can reuse the job.
    resp2 = await service_client.post(
        "/v1/capture", json={"link": link}, headers={_CLIENT_IP_HEADER: _OTHER_CLIENT_IP}
    )
    assert resp2.status == 202
    job2 = resp2.json()
    assert job2["uuid"] == job1["uuid"]

    for _ in range(3):
        await asyncio.sleep(_LINK_RATELIMIT_SEC)
        resp3 = await service_client.post(
            "/v1/capture", json={"link": link}, headers={_CLIENT_IP_HEADER: _THIRD_CLIENT_IP}
        )
        assert resp3.status == 202
        job3 = resp3.json()
        if job3["uuid"] != job1["uuid"]:
            return

    raise AssertionError("link ratelimit did not expire")


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_ratelimit])
async def test_create_capture_rejects_same_ip_during_ratelimit(service_client):
    first_link = f"https://{TEST_HOST}/ip-ratelimit-first"
    second_link = f"https://{TEST_HOST}/ip-ratelimit-second"
    headers = {_CLIENT_IP_HEADER: _CLIENT_IP}

    resp1 = await service_client.post("/v1/capture", json={"link": first_link}, headers=headers)
    assert resp1.status == 202

    resp2 = await service_client.post("/v1/capture", json={"link": second_link}, headers=headers)
    assert resp2.status == 429
    assert resp2.headers["Retry-After"]
    body = resp2.json()
    assert body["error"]["message"] == "client IP rate limited"


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_ratelimit])
async def test_create_capture_accepts_ipv6_client_ip(service_client):
    first_link = f"https://{TEST_HOST}/ip-ratelimit-ipv6-first"
    second_link = f"https://{TEST_HOST}/ip-ratelimit-ipv6-second"
    headers = {_CLIENT_IP_HEADER: _CLIENT_IPV6_BRACKETED}

    resp1 = await service_client.post("/v1/capture", json={"link": first_link}, headers=headers)
    assert resp1.status == 202

    resp2 = await service_client.post("/v1/capture", json={"link": second_link}, headers=headers)
    assert resp2.status == 429
    assert resp2.json()["error"]["message"] == "client IP rate limited"


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_ratelimit])
async def test_create_capture_ip_ratelimit_expires(service_client):
    first_link = f"https://{TEST_HOST}/ip-ratelimit-expiry-first"
    second_link = f"https://{TEST_HOST}/ip-ratelimit-expiry-second"
    headers = {_CLIENT_IP_HEADER: _CLIENT_IP}

    resp1 = await service_client.post("/v1/capture", json={"link": first_link}, headers=headers)
    assert resp1.status == 202

    while True:
        resp2 = await service_client.post(
            "/v1/capture", json={"link": second_link}, headers=headers
        )
        if resp2.status == 429:
            break
        assert resp2.status == 202
        assert resp2.json()["uuid"] != resp1.json()["uuid"]

    retry_after = int(resp2.headers["Retry-After"])
    await asyncio.sleep(retry_after)

    while True:
        allowed = await service_client.post(
            "/v1/capture", json={"link": second_link}, headers=headers
        )
        if allowed.status == 202:
            assert allowed.json()["uuid"] != resp1.json()["uuid"]
            return
        assert allowed.status == 429
        retry_after = int(allowed.headers["Retry-After"])
        await asyncio.sleep(retry_after)

    raise AssertionError("unreachable")


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_ratelimit])
async def test_ip_ratelimit_applies_to_read_crud_operations(service_client):
    link = f"https://{TEST_HOST}/ip-ratelimit-read-crud"
    headers = {_CLIENT_IP_HEADER: _CLIENT_IP}

    resp1 = await service_client.post("/v1/capture", json={"link": link}, headers=headers)
    assert resp1.status == 202
    job_id = resp1.json()["uuid"]

    resp2 = await service_client.get("/v1/capture", params={"link": link}, headers=headers)
    assert resp2.status == 429
    assert resp2.headers["Retry-After"]
    assert resp2.json()["error"]["message"] == "client IP rate limited"

    resp3 = await service_client.get(f"/v1/capture/jobs/{job_id}", headers=headers)
    assert resp3.status == 429
    assert resp3.headers["Retry-After"]
    body = resp3.json()
    assert body["uuid"] == job_id
    assert body["retry_after_sec"] >= 1
    assert body["error"]["message"] == "client IP rate limited"


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_ratelimit])
async def test_create_capture_different_ip_can_reuse_link_job(service_client):
    link = f"https://{TEST_HOST}/ip-ratelimit-link-reuse"

    resp1 = await service_client.post(
        "/v1/capture",
        json={"link": link},
        headers={_CLIENT_IP_HEADER: _CLIENT_IP},
    )
    assert resp1.status == 202

    resp2 = await service_client.post(
        "/v1/capture",
        json={"link": link},
        headers={_CLIENT_IP_HEADER: _OTHER_CLIENT_IP},
    )
    assert resp2.status == 202
    assert resp2.json()["uuid"] == resp1.json()["uuid"]


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_ratelimit])
async def test_create_capture_rejects_missing_or_invalid_client_ip_header(service_client):
    link = f"https://{TEST_HOST}/ip-ratelimit-invalid-header"

    missing = await service_client.post("/v1/capture", json={"link": link})
    assert missing.status == 400
    assert missing.json()["error"]["message"] == "invalid client IP"

    invalid = await service_client.post(
        "/v1/capture",
        json={"link": link},
        headers={_CLIENT_IP_HEADER: f"{_CLIENT_IP}, {_OTHER_CLIENT_IP}"},
    )
    assert invalid.status == 400
    assert invalid.json()["error"]["message"] == "invalid client IP"
