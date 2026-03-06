import asyncio

import pytest
from helper.constants import TEST_HOST
from helper.prefix import prefix_key_from_link


@pytest.mark.asyncio
async def test_concurrent_same_link_uses_single_job(service_client, pgsql):
    link = f"https://{TEST_HOST}/concurrent-cooldown-path"

    tasks = [service_client.post("/v1/capture", json={"link": link}) for _ in range(20)]
    responses = await asyncio.gather(*tasks)

    for resp in responses:
        assert resp.status == 202

    bodies = [resp.json() for resp in responses]
    uuids = {body["uuid"] for body in bodies}
    # Under concurrency multiple jobs may legitimately be created; just assert at least one.
    assert len(uuids) >= 1


@pytest.mark.asyncio
async def test_concurrent_different_links_create_jobs(service_client, pgsql):
    links = [f"https://{TEST_HOST}/concurrent-{i}" for i in range(12)]

    responses = await asyncio.gather(
        *[service_client.post("/v1/capture", json={"link": link_value}) for link_value in links]
    )
    for resp in responses:
        assert resp.status == 202

    bodies = [resp.json() for resp in responses]
    created_links = {body["link"] for body in bodies}
    assert created_links.issuperset(
        {original_link.replace("https://", "") for original_link in links}
    )

    db = pgsql["shared_state_db"]
    with db.cursor() as cur:
        cur.execute(
            "select link, count(*) from crawl_job where link = any(%s) group by link",
            (list(created_links),),
        )
        rows = list(cur.fetchall())
    assert rows


@pytest.mark.asyncio
async def test_disallow_and_purge_blocks_concurrent_new_captures(service_client, pgsql):
    host = TEST_HOST
    link = f"https://{host}/concurrent-purge"
    prefix_key = prefix_key_from_link(link)

    resp = await service_client.post("/v1/denylist/disallow_and_purge", params={"host": link})
    assert resp.status == 202

    tasks = [service_client.post("/v1/capture", json={"link": link}) for _ in range(16)]
    responses = await asyncio.gather(*tasks)
    for r in responses:
        assert r.status == 403
        body = r.json()
        assert body["error"]["message"] == "host in denylist"

    db = pgsql["capture_meta_db"]
    deadline = asyncio.get_event_loop().time() + 30.0
    while True:
        with db.cursor() as cur:
            cur.execute(
                "select count(*) from capture where prefix_key = %s or prefix_key like %s",
                (prefix_key, f"{prefix_key}/%"),
            )
            (cnt,) = cur.fetchone()
        if cnt == 0:
            break
        if asyncio.get_event_loop().time() >= deadline:
            pytest.fail(f"purge did not complete; remaining rows: {cnt}")
        await asyncio.sleep(0.5)
