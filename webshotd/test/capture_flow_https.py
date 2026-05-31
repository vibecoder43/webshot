import pytest
from helper.capture_flow import (
    _assert_missing_job_fields,
    _capture_and_wait,
    _probe_replay,
    _wacz_archive_text,
    _wacz_cdx_statuses_for_root_url,
    _wacz_cdx_statuses_for_url,
    _wacz_entries,
)
from helper.config_hooks import enable_allowlist_only, enable_https_only
from helper.constants import TEST_HOST, UNTRUSTED_TEST_HOST
from helper.waiters import wait_for_job_status


@pytest.mark.uservice_oneshot(config_hooks=[enable_allowlist_only])
@pytest.mark.asyncio
async def test_allowlist_only_blocks_non_allowlisted_redirect_target(
    service_client, monitor_client
):
    seed = f"https://{TEST_HOST}/redirect-seed"

    resp = await monitor_client.post("/v1/allowlist/add", json={"link": seed})
    assert resp.status == 204

    resp = await service_client.post("/v1/capture", json={"link": seed})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    job = await wait_for_job_status(service_client, job_id, expected_status="failed")

    assert "started_at" in job
    assert "finished_at" in job
    _assert_missing_job_fields(job, "result", "result_created_at")
    assert job["error"]["error"]["message"] == "capture failed"


@pytest.mark.asyncio
async def test_https_first_succeeds_when_http_fails(
    service_client, browser_probe, download_wacz, service_baseurl
):
    link = f"http://{TEST_HOST}/https-first-http-fails"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    await _probe_replay(browser_probe, service_baseurl, job_id)

    wacz = await download_wacz(job_id)
    assert 200 in _wacz_cdx_statuses_for_url(wacz, f"https://{TEST_HOST}/https-first-http-fails")
    assert not _wacz_cdx_statuses_for_url(wacz, f"http://{TEST_HOST}/https-first-http-fails")


@pytest.mark.uservice_oneshot(config_hooks=[enable_https_only])
@pytest.mark.asyncio
async def test_https_only_accepts_http_seed_and_crawls_https(
    service_client, browser_probe, download_wacz, service_baseurl
):
    link = f"http://{TEST_HOST}/https-first-http-fails"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    await _probe_replay(browser_probe, service_baseurl, job_id)

    wacz = await download_wacz(job_id)
    assert 200 in _wacz_cdx_statuses_for_url(wacz, f"https://{TEST_HOST}/https-first-http-fails")
    assert not _wacz_cdx_statuses_for_url(wacz, f"http://{TEST_HOST}/https-first-http-fails")


@pytest.mark.asyncio
async def test_https_first_falls_back_to_http_when_https_no_response(
    service_client, browser_probe, download_wacz, service_baseurl
):
    link = f"http://{TEST_HOST}/http-fallback-success"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    await _probe_replay(browser_probe, service_baseurl, job_id)

    wacz = await download_wacz(job_id)
    assert 200 in _wacz_cdx_statuses_for_url(wacz, f"http://{TEST_HOST}/http-fallback-success")
    assert not _wacz_cdx_statuses_for_url(wacz, f"https://{TEST_HOST}/http-fallback-success")


@pytest.mark.uservice_oneshot(config_hooks=[enable_https_only])
@pytest.mark.asyncio
async def test_https_only_does_not_fall_back_to_http(service_client):
    link = f"http://{TEST_HOST}/http-fallback-success"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="failed")


@pytest.mark.asyncio
async def test_https_first_falls_back_to_http_and_fails_when_http_fails(service_client):
    link = f"http://{TEST_HOST}/http-fallback-fail"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="failed")


@pytest.mark.asyncio
async def test_capture_downgrades_untrusted_https_certificate_to_http(
    service_client, browser_probe, download_wacz, service_baseurl
):
    https_link = f"https://{UNTRUSTED_TEST_HOST}/"

    resp = await service_client.post("/v1/capture", json={"link": https_link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    job = await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    assert "started_at" in job
    assert "finished_at" in job
    assert "result_created_at" in job
    assert job["result"]["uuid"] == job_id
    resp = await service_client.get(f"/v1/capture/{job_id}")
    assert resp.status == 200
    assert resp.json()["storage_url"].endswith(f"{job_id}.wacz")

    await _probe_replay(browser_probe, service_baseurl, job_id)

    wacz = await download_wacz(job_id)
    archive_text = _wacz_archive_text(wacz)
    assert 200 in _wacz_cdx_statuses_for_root_url(wacz, f"http://{UNTRUSTED_TEST_HOST}/")
    assert not _wacz_cdx_statuses_for_root_url(wacz, f"https://{UNTRUSTED_TEST_HOST}/")
    assert "WARC-Target-URI: http://untrusted.test-target/" in archive_text
    assert "untrusted" in archive_text


@pytest.mark.asyncio
async def test_sequential_captures_do_not_reuse_browser_state(
    service_client, browser_probe, download_wacz, service_baseurl
):
    first_job_id, _first_job = await _capture_and_wait(
        service_client, f"https://{TEST_HOST}/state-write"
    )
    second_job_id, _second_job = await _capture_and_wait(
        service_client, f"https://{TEST_HOST}/state-read"
    )

    assert first_job_id != second_job_id

    await _probe_replay(browser_probe, service_baseurl, second_job_id)

    first_wacz = await download_wacz(first_job_id)
    second_wacz = await download_wacz(second_job_id)
    assert "reused_browser=false" in _wacz_entries(first_wacz)["logs/stdout.log"].decode("utf-8")
    assert "reused_browser=false" in _wacz_entries(second_wacz)["logs/stdout.log"].decode("utf-8")
    archive_text = _wacz_archive_text(second_wacz)
    assert "seen=first" not in archive_text
    assert '"localStorage":"first"' not in archive_text
    assert '"sessionStorage":"first"' not in archive_text
    assert '"localStorage":null' in archive_text
    assert '"sessionStorage":null' in archive_text
