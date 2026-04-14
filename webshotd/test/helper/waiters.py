import asyncio

import pytest

_DEFAULT_POLL_DELAY = 0.2
_DEFAULT_JOB_TIMEOUT = 60.0
_DEFAULT_PURGE_TIMEOUT = 30.0


async def wait_for_job_status(
    service_client,
    job_id: str,
    *,
    expected_status: str,
    timeout: float = _DEFAULT_JOB_TIMEOUT,
    delay: float = _DEFAULT_POLL_DELAY,
) -> dict[str, object]:
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while True:
        status_resp = await service_client.get(f"/v1/capture/jobs/{job_id}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == expected_status:
            return job
        if job["status"] in {"failed", "succeeded"} and job["status"] != expected_status:
            pytest.fail(f"job reached unexpected terminal state: {job}")
        if loop.time() >= deadline:
            pytest.fail(f"job did not reach {expected_status!r} in time: {job}")
        await asyncio.sleep(delay)


async def wait_for_purge(
    db,
    prefix_key: str,
    *,
    timeout: float = _DEFAULT_PURGE_TIMEOUT,
    delay: float = _DEFAULT_POLL_DELAY,
) -> None:
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while True:
        with db.cursor() as cur:
            cur.execute(
                "select count(*) from capture where prefix_key = %s or prefix_key like %s",
                (prefix_key, f"{prefix_key}/%"),
            )
            (cnt,) = cur.fetchone()
        if cnt == 0:
            return
        if loop.time() >= deadline:
            raise AssertionError(f"purge did not complete; remaining rows: {cnt}")
        await asyncio.sleep(delay)
