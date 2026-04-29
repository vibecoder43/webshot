import asyncio

import pytest
from helper.constants import TEST_HOST
from helper.prefix import prefix_key_from_link
from helper.waiters import wait_for_purge

_SAME_LINK_REQUEST_COUNT = 6
_DIFFERENT_LINK_REQUEST_COUNT = 4
_DENYLIST_REQUEST_COUNT = 6


@pytest.mark.asyncio
async def test_concurrent_same_link_uses_single_job(service_client, pgsql):
    link = f"https://{TEST_HOST}/concurrent-cooldown-path"

    tasks = [
        service_client.post("/v1/capture", json={"link": link})
        for _ in range(_SAME_LINK_REQUEST_COUNT)
    ]
    responses = await asyncio.gather(*tasks)

    for resp in responses:
        assert resp.status == 202

    bodies = [resp.json() for resp in responses]
    uuids = {body["uuid"] for body in bodies}
    assert len(uuids) == 1

    db = pgsql["shared_state_db"]
    with db.cursor() as cur:
        cur.execute("select count(*) from crawl_job where link = %s", (bodies[0]["link"],))
        (cnt,) = cur.fetchone()
    assert cnt == 1


@pytest.mark.asyncio
async def test_concurrent_different_links_create_jobs(service_client, pgsql):
    links = [f"https://{TEST_HOST}/concurrent-{i}" for i in range(_DIFFERENT_LINK_REQUEST_COUNT)]

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
async def test_disallow_and_purge_blocks_concurrent_new_captures(
    service_client, monitor_client, pgsql
):
    host = TEST_HOST
    link = f"https://{host}/concurrent-purge"
    prefix_key = prefix_key_from_link(link)

    resp = await monitor_client.post("/v1/denylist/disallow_and_purge", json={"link": link})
    assert resp.status == 202

    tasks = [
        service_client.post("/v1/capture", json={"link": link})
        for _ in range(_DENYLIST_REQUEST_COUNT)
    ]
    responses = await asyncio.gather(*tasks)
    for r in responses:
        assert r.status == 403
        body = r.json()
        assert body["error"]["message"] == "host in denylist"

    db = pgsql["capture_meta_db"]
    await wait_for_purge(db, prefix_key)
