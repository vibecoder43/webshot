import uuid

from helper.constants import TEST_HOST


async def test_create_capture_respects_link_cooldown(service_client, pgsql):
    link = f"https://{TEST_HOST}/cooldown-path"

    resp1 = await service_client.post("/v1/capture", json={"link": link})
    assert resp1.status == 202
    job1 = resp1.json()
    job1_id = uuid.UUID(job1["uuid"])

    # Second request for the same link should reuse the existing job
    resp2 = await service_client.post("/v1/capture", json={"link": link})
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

    resp3 = await service_client.post("/v1/capture", json={"link": link})
    assert resp3.status == 202
    job3 = resp3.json()
    assert job3["uuid"] != job1["uuid"]
