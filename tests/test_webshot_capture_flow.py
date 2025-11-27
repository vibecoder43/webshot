import asyncio
import uuid
from urllib.parse import urlparse

import pytest


def _reverse_host_from_link(link: str) -> str:
    parsed = urlparse(link if "://" in link else f"https://{link}")
    return parsed.hostname[::-1] if parsed.hostname else ""


async def _wait_for_purge(db, host_rev: str, timeout: float = 30.0, delay: float = 0.5):
    deadline = asyncio.get_event_loop().time() + timeout
    while True:
        with db.cursor() as cur:
            cur.execute("select count(*) from webshot where host_rev = %s", (host_rev,))
            (cnt,) = cur.fetchone()
        if cnt == 0:
            return
        if asyncio.get_event_loop().time() >= deadline:
            raise AssertionError(f"purge did not complete; remaining rows: {cnt}")
        await asyncio.sleep(delay)


@pytest.mark.asyncio
async def test_capture_and_query_example_com(service_client, pgsql):
    link = "https://example.com/webshot-capture-path"

    # Create capture
    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 202
    job_body = resp.json()
    uuid_str = job_body["uuid"]
    normalized_link = job_body["link"]

    # Wait for job completion (allow up to ~60s)
    for _ in range(120):
        status_resp = await service_client.get(f"/v1/webshot/jobs/{uuid_str}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == "succeeded":
            assert job["result"]["uuid"] == uuid_str
            normalized_link = job["result"]["link"]
            break
        if job["status"] == "failed":
            pytest.fail(f"job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete in time")

    # Resolve by id (redirect)
    resp = await service_client.get(f"/v1/webshot/{uuid_str}", allow_redirects=False)
    assert resp.status == 302
    loc = resp.headers.get("Location", "")
    assert loc.endswith(uuid_str)

    # List by exact link
    resp = await service_client.get("/v1/webshot", params={"link": normalized_link})
    assert resp.status == 200
    items = resp.json()["items"]
    assert any(item["uuid"] == uuid_str for item in items)

    # List by prefix (use host prefix)
    prefix = urlparse(normalized_link).hostname or "example.com"
    resp = await service_client.get("/v1/webshot/prefix", params={"prefix": prefix})
    assert resp.status == 200
    prefix_items = resp.json()["items"]
    assert any(
        item["uuid"] == uuid_str and item["link"] == normalized_link for item in prefix_items
    )

    # Verify DB row
    db = pgsql["capture_meta_db_schema"]
    with db.cursor() as cur:
        cur.execute(
            "select location, host_rev from webshot where id = %s",
            (uuid.UUID(uuid_str),),
        )
        row = cur.fetchone()
    assert row is not None
    location, host_rev = row
    assert location.endswith(uuid_str)
    assert host_rev == _reverse_host_from_link(normalized_link)


@pytest.mark.asyncio
async def test_disallow_and_purge_blocks_new_captures(service_client, pgsql):
    host = "example.com"
    link = f"https://{host}/"
    host_rev = host[::-1]

    # Ensure at least one capture exists before purge
    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 202
    first_job_id = resp.json()["uuid"]
    for _ in range(60):
        status_resp = await service_client.get(f"/v1/webshot/jobs/{first_job_id}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == "succeeded":
            break
        if job["status"] == "failed":
            pytest.fail(f"initial job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("initial job did not complete")

    # Disallow and purge
    resp = await service_client.post("/v1/disallow-and-purge", params={"host": link})
    assert resp.status == 202

    # Wait for purge to remove rows for this host
    db = pgsql["capture_meta_db_schema"]
    await _wait_for_purge(db, host_rev)

    # Attempt new capture should be blocked by denylist
    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 403
    err = resp.json()["error"]["message"]
    assert err == "host in denylist"
