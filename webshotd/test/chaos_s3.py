import uuid

import pytest
from helper.constants import TEST_HOST
from helper.s3_gate_config import enable_s3_gate
from helper.waiters import wait_for_job_status

pytestmark = pytest.mark.uservice_oneshot(config_hooks=[enable_s3_gate])


@pytest.fixture
async def service_client_with_s3_gate(s3_gate_ready, service_client):
    return service_client


@pytest.mark.asyncio
async def test_s3_outage_marks_job_failed(service_client_with_s3_gate, s3_gate, pgsql):
    await s3_gate.sockets_close()
    await s3_gate.stop_accepting()

    link = f"https://{TEST_HOST}/chaos-s3-error"
    service_client = service_client_with_s3_gate
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    job = await wait_for_job_status(service_client, job_id, expected_status="failed")
    assert job["status"] == "failed"

    db = pgsql["capture_meta_db"]
    with db.cursor() as cur:
        cur.execute("select 1 from capture where id = %s", (uuid.UUID(job_id),))
        assert cur.fetchone() is None

    await s3_gate.to_server_pass()
    await s3_gate.to_client_pass()
    s3_gate.start_accepting()


@pytest.mark.asyncio
async def test_recent_failed_job_is_reused_during_ratelimit(service_client_with_s3_gate, s3_gate):
    await s3_gate.sockets_close()
    await s3_gate.stop_accepting()

    link = f"https://{TEST_HOST}/chaos-s3-failed-reuse"
    service_client = service_client_with_s3_gate
    resp1 = await service_client.post("/v1/capture", json={"link": link})
    assert resp1.status == 202
    job1_id = resp1.json()["uuid"]

    job1 = await wait_for_job_status(service_client, job1_id, expected_status="failed")
    assert job1["status"] == "failed"

    await s3_gate.to_server_pass()
    await s3_gate.to_client_pass()
    s3_gate.start_accepting()

    resp2 = await service_client.post("/v1/capture", json={"link": link})
    assert resp2.status == 202
    job2 = resp2.json()
    assert job2["uuid"] == job1_id
    assert job2["status"] == "failed"


@pytest.mark.asyncio
async def test_s3_recovers_after_outage(service_client_with_s3_gate, s3_gate):
    await s3_gate.to_server_pass()
    await s3_gate.to_client_pass()
    s3_gate.start_accepting()

    link = f"https://{TEST_HOST}/chaos-s3-recover"
    service_client = service_client_with_s3_gate
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")
