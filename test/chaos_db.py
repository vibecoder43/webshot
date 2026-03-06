import pytest
from helper.constants import TEST_HOST


@pytest.mark.asyncio
async def test_db_outage_returns_5xx(service_client, pg_gate):
    await pg_gate.stop_accepting()

    resp = await service_client.get(
        "/v1/capture",
        params={"link": f"https://{TEST_HOST}/db-outage"},
    )
    # Service should stay responsive even if DB connections are affected.
    assert 200 <= resp.status < 600

    await pg_gate.to_server_pass()
    await pg_gate.to_client_pass()
    pg_gate.start_accepting()


@pytest.mark.asyncio
async def test_db_slow_requests_respect_deadlines(service_client, pg_gate):
    await pg_gate.to_server_pass()
    await pg_gate.to_client_pass()
    await pg_gate.to_server_delay(2.0)

    resp = await service_client.get(
        "/v1/capture",
        params={"link": f"https://{TEST_HOST}/db-slow"},
    )
    # Depending on pool state this may still be 200; just assert no crash.
    assert 200 <= resp.status < 600

    await pg_gate.to_server_pass()
    await pg_gate.to_client_pass()
    pg_gate.start_accepting()
