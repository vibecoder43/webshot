import uuid

import pytest
from helper.constants import TEST_HOST

_CLIENT_IP_HEADER = "X-Test-Client-IP"
_CLIENT_IP = "198.51.100.9"
_OTHER_CLIENT_IP = "198.51.100.10"
_THIRD_CLIENT_IP = "198.51.100.11"


def _enable_ip_cooldown(_config_yaml, config_vars):
    config_vars["ip_cooldown_ms"] = 500
    config_vars["client_ip_source"] = "trusted_header"
    config_vars["client_ip_header_name"] = _CLIENT_IP_HEADER


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_cooldown])
async def test_create_capture_respects_link_cooldown(service_client, pgsql):
    link = f"https://{TEST_HOST}/cooldown-path"

    resp1 = await service_client.post(
        "/v1/capture", json={"link": link}, headers={_CLIENT_IP_HEADER: _CLIENT_IP}
    )
    assert resp1.status == 202
    job1 = resp1.json()
    job1_id = uuid.UUID(job1["uuid"])

    # A different client IP is not blocked by IP cooldown, so link cooldown can reuse the job.
    resp2 = await service_client.post(
        "/v1/capture", json={"link": link}, headers={_CLIENT_IP_HEADER: _OTHER_CLIENT_IP}
    )
    assert resp2.status == 202
    job2 = resp2.json()
    assert job2["uuid"] == job1["uuid"]

    # Move the existing job far into the past so that cooldown no longer applies
    db = pgsql["shared_state_db"]
    with db.cursor() as cur:
        cur.execute(
            "update crawl_job set created_at = created_at - interval '1 day' where id = %s",
            (job1_id,),
        )

    resp3 = await service_client.post(
        "/v1/capture", json={"link": link}, headers={_CLIENT_IP_HEADER: _THIRD_CLIENT_IP}
    )
    assert resp3.status == 202
    job3 = resp3.json()
    assert job3["uuid"] != job1["uuid"]


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_cooldown])
async def test_create_capture_rejects_same_ip_during_cooldown(service_client, pgsql):
    first_link = f"https://{TEST_HOST}/ip-cooldown-first"
    second_link = f"https://{TEST_HOST}/ip-cooldown-second"
    headers = {_CLIENT_IP_HEADER: _CLIENT_IP}

    resp1 = await service_client.post("/v1/capture", json={"link": first_link}, headers=headers)
    assert resp1.status == 202

    resp2 = await service_client.post("/v1/capture", json={"link": second_link}, headers=headers)
    assert resp2.status == 429
    assert resp2.headers["Retry-After"]
    body = resp2.json()
    assert body["error"]["message"] == "client IP in cooldown"

    db = pgsql["shared_state_db"]
    with db.cursor() as cur:
        cur.execute("select count(*) from client_ip_cooldown where client_ip = %s", (_CLIENT_IP,))
        (cnt,) = cur.fetchone()
    assert cnt == 1


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_cooldown])
async def test_create_capture_ip_cooldown_expires(service_client, pgsql):
    first_link = f"https://{TEST_HOST}/ip-cooldown-expiry-first"
    second_link = f"https://{TEST_HOST}/ip-cooldown-expiry-second"
    headers = {_CLIENT_IP_HEADER: _CLIENT_IP}

    resp1 = await service_client.post("/v1/capture", json={"link": first_link}, headers=headers)
    assert resp1.status == 202

    db = pgsql["shared_state_db"]
    with db.cursor() as cur:
        cur.execute(
            "update client_ip_cooldown set expires_at = now() - interval '1 second' "
            "where client_ip = %s",
            (_CLIENT_IP,),
        )

    resp2 = await service_client.post("/v1/capture", json={"link": second_link}, headers=headers)
    assert resp2.status == 202
    assert resp2.json()["uuid"] != resp1.json()["uuid"]


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_cooldown])
async def test_ip_cooldown_applies_to_read_crud_operation(service_client):
    link = f"https://{TEST_HOST}/ip-cooldown-read-crud"
    headers = {_CLIENT_IP_HEADER: _CLIENT_IP}

    resp1 = await service_client.post("/v1/capture", json={"link": link}, headers=headers)
    assert resp1.status == 202

    resp2 = await service_client.get("/v1/capture", params={"link": link}, headers=headers)
    assert resp2.status == 429
    assert resp2.headers["Retry-After"]
    assert resp2.json()["error"]["message"] == "client IP in cooldown"


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_cooldown])
async def test_create_capture_different_ip_can_reuse_link_job(service_client):
    link = f"https://{TEST_HOST}/ip-cooldown-link-reuse"

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


@pytest.mark.uservice_oneshot(config_hooks=[_enable_ip_cooldown])
async def test_create_capture_rejects_missing_or_invalid_client_ip_header(service_client):
    link = f"https://{TEST_HOST}/ip-cooldown-invalid-header"

    missing = await service_client.post("/v1/capture", json={"link": link})
    assert missing.status == 400
    assert missing.json()["error"]["message"] == "invalid client ip"

    invalid = await service_client.post(
        "/v1/capture",
        json={"link": link},
        headers={_CLIENT_IP_HEADER: f"{_CLIENT_IP}, {_OTHER_CLIENT_IP}"},
    )
    assert invalid.status == 400
    assert invalid.json()["error"]["message"] == "invalid client ip"
