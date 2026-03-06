import asyncio
import uuid

import pytest
from helper.constants import TEST_HOST


@pytest.mark.asyncio
async def test_s3_outage_marks_job_failed(service_client, s3_gate, pgsql):
    await s3_gate.sockets_close()
    await s3_gate.stop_accepting()

    link = f"https://{TEST_HOST}/chaos-s3-failure"
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    job = None
    for _ in range(60):
        status_resp = await service_client.get(f"/v1/capture/jobs/{job_id}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] in ("succeeded", "failed"):
            break
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete")

    assert job is not None
    assert job["status"] == "failed"

    db = pgsql["capture_meta_db"]
    with db.cursor() as cur:
        cur.execute("select 1 from capture where id = %s", (uuid.UUID(job_id),))
        assert cur.fetchone() is None

    await s3_gate.to_server_pass()
    await s3_gate.to_client_pass()
    s3_gate.start_accepting()


@pytest.mark.asyncio
async def test_s3_recovers_after_outage(service_client, s3_gate):
    await s3_gate.to_server_pass()
    await s3_gate.to_client_pass()
    s3_gate.start_accepting()

    link = f"https://{TEST_HOST}/chaos-s3-recover"
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    for _ in range(120):
        status_resp = await service_client.get(f"/v1/capture/jobs/{job_id}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == "succeeded":
            break
        if job["status"] == "failed":
            pytest.fail(f"job failed unexpectedly: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not succeed after S3 recovery")
