import uuid

import pytest
from helper.constants import TEST_HOST
from helper.waiters import wait_for_job_status


def _patch_dedup_crawler_timeouts(config_yaml, config_vars):
    # Dedup performs two full crawl jobs in one test; give only this test a bit more crawl headroom.
    del config_yaml
    config_vars["crawler_run_timeout_sec"] = 14
    config_vars["crawler_job_overhead_timeout_sec"] = 8
    config_vars["crawler_devtools_startup_timeout_sec"] = 10
    config_vars["crawler_cdp_handshake_timeout_sec"] = 6
    config_vars["crawler_cdp_command_timeout_sec"] = 6


@pytest.mark.uservice_oneshot(config_hooks=[_patch_dedup_crawler_timeouts])
@pytest.mark.asyncio
async def test_dedup_reuses_earlier_capture_uuid(service_client, pgsql):
    link = f"https://{TEST_HOST}/dedup-path"
    dedup_timeout = 42.0

    resp1 = await service_client.post("/v1/capture", json={"link": link})
    assert resp1.status == 202
    job1_id = resp1.json()["uuid"]
    job1 = await wait_for_job_status(
        service_client, job1_id, expected_status="succeeded", timeout=dedup_timeout
    )
    capture_id = job1["result"]["uuid"]
    normalized_link = job1["result"]["link"]

    # Ensure a new job is created even if link ratelimit is enabled.
    db = pgsql["shared_state_db"]
    with db.cursor() as cur:
        cur.execute(
            "update crawl_job set created_at = created_at - interval '1 day' where id = %s",
            (uuid.UUID(job1_id),),
        )

    resp2 = await service_client.post("/v1/capture", json={"link": link})
    assert resp2.status == 202
    job2_id = resp2.json()["uuid"]
    assert job2_id != job1_id
    job2 = await wait_for_job_status(
        service_client, job2_id, expected_status="succeeded", timeout=dedup_timeout
    )

    assert job2["result"]["uuid"] == capture_id
    assert job2["result"]["link"] == normalized_link

    # Verify the deduped job did not create an additional capture row.
    db = pgsql["capture_meta_db"]
    with db.cursor() as cur:
        cur.execute("select count(*) from capture where link = %s", (normalized_link,))
        (cnt,) = cur.fetchone()
    assert cnt == 1
