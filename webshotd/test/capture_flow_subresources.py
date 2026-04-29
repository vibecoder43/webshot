import pytest
from helper.capture_flow import (
    _capture_and_wait,
    _enable_allowlist_only,
    _enable_https_only,
    _probe_replay,
    _wacz_archive_text,
    _wacz_cdx_statuses_for_url,
)
from helper.constants import TEST_ASSET_HOST, TEST_HOST
from helper.waiters import wait_for_job_status


@pytest.mark.asyncio
async def test_capture_preserves_post_subresource_requests(service_client, download_wacz):
    link = f"https://{TEST_HOST}/with-post-subresource"
    job_id, _job = await _capture_and_wait(service_client, link)

    wacz = await download_wacz(job_id)
    submit_url = f"https://{TEST_HOST}/submit?source=page"
    submit_statuses = _wacz_cdx_statuses_for_url(wacz, submit_url)
    assert 200 in submit_statuses

    archive_text = _wacz_archive_text(wacz)
    assert "WARC-Target-URI: https://test-target/submit?source=page" in archive_text
    assert "POST /submit?source=page HTTP/1.1" in archive_text


@pytest.mark.asyncio
async def test_capture_preserves_head_subresource_requests(service_client, download_wacz):
    link = f"https://{TEST_HOST}/with-head-subresource"
    job_id, _job = await _capture_and_wait(service_client, link)

    wacz = await download_wacz(job_id)
    metadata_url = f"https://{TEST_HOST}/metadata?source=page"
    metadata_statuses = _wacz_cdx_statuses_for_url(wacz, metadata_url)
    assert 200 in metadata_statuses

    archive_text = _wacz_archive_text(wacz)
    assert "WARC-Target-URI: https://test-target/metadata?source=page" in archive_text
    assert "HEAD /metadata?source=page HTTP/1.1" in archive_text


@pytest.mark.asyncio
async def test_capture_preserves_redirected_subresource_hops(
    service_client, browser_probe, download_wacz, service_baseurl
):
    link = f"https://{TEST_HOST}/with-redirected-asset"
    job_id, _job = await _capture_and_wait(service_client, link)

    await _probe_replay(browser_probe, service_baseurl, job_id)

    wacz = await download_wacz(job_id)
    archive_text = _wacz_archive_text(wacz)
    assert 302 in _wacz_cdx_statuses_for_url(wacz, f"https://{TEST_HOST}/redirect-script.js")
    assert 200 in _wacz_cdx_statuses_for_url(wacz, f"https://{TEST_HOST}/script-final.js")
    assert "location: /script-final.js" in archive_text
    assert "window.__redirectedAssetLoaded = true;" in archive_text


@pytest.mark.asyncio
async def test_denylist_blocks_subresource_fetch(
    service_client, monitor_client, browser_probe, service_baseurl
):
    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        json={"link": f"https://{TEST_HOST}/denylist/style.css"},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-subresource-denylist"
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, service_baseurl, job_id)
    assert any("denylist" in entry for entry in replay["console"])


@pytest.mark.asyncio
async def test_regular_mode_allowlist_overrides_denylisted_subresource_fetch(
    service_client, monitor_client, browser_probe, service_baseurl
):
    script = f"https://{TEST_HOST}/denylist/script.js"

    allow_resp = await monitor_client.post("/v1/allowlist/add", json={"link": script})
    assert allow_resp.status == 204

    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        json={"link": script},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-subresource-denylist"
    job_id, _job = await _capture_and_wait(service_client, link)

    replay = await _probe_replay(browser_probe, service_baseurl, job_id)
    assert any("denylist" in entry for entry in replay["console"])


@pytest.mark.uservice_oneshot(config_hooks=[_enable_allowlist_only])
@pytest.mark.asyncio
async def test_allowlist_only_fetches_allowlisted_subresources(
    service_client, monitor_client, browser_probe, download_wacz, service_baseurl
):
    seed = f"https://{TEST_HOST}/with-subresource"
    style = f"https://{TEST_HOST}/style.css"
    script = f"https://{TEST_HOST}/script.js"
    for link in [seed, style, script]:
        resp = await monitor_client.post("/v1/allowlist/add", json={"link": link})
        assert resp.status == 204

    job_id, _job = await _capture_and_wait(service_client, seed)

    replay = await _probe_replay(browser_probe, service_baseurl, job_id)
    assert any("ok" in entry for entry in replay["console"])

    wacz = await download_wacz(job_id)
    assert 200 in _wacz_cdx_statuses_for_url(wacz, style)
    assert 200 in _wacz_cdx_statuses_for_url(wacz, script)


@pytest.mark.uservice_oneshot(config_hooks=[_enable_allowlist_only])
@pytest.mark.asyncio
async def test_allowlist_only_blocks_non_allowlisted_subresources(
    service_client, monitor_client, browser_probe, download_wacz, service_baseurl
):
    seed = f"https://{TEST_HOST}/with-https-asset-subresource"
    asset_script = f"https://{TEST_ASSET_HOST}/asset.js"

    resp = await monitor_client.post("/v1/allowlist/add", json={"link": seed})
    assert resp.status == 204

    job_id, _job = await _capture_and_wait(service_client, seed)

    replay = await _probe_replay(browser_probe, service_baseurl, job_id)
    assert not any("asset" in entry for entry in replay["console"])

    wacz = await download_wacz(job_id)
    assert 403 in _wacz_cdx_statuses_for_url(wacz, asset_script)


@pytest.mark.asyncio
async def test_capture_fetches_https_subresource_assets(
    service_client, browser_probe, service_baseurl
):
    link = f"https://{TEST_HOST}/with-https-asset-subresource"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, service_baseurl, job_id)
    assert any("asset" in entry for entry in replay["console"])


@pytest.mark.uservice_oneshot(config_hooks=[_enable_https_only])
@pytest.mark.asyncio
async def test_https_only_blocks_http_subresource_fetch(
    service_client, browser_probe, download_wacz, service_baseurl
):
    link = f"https://{TEST_HOST}/with-http-subresource"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, service_baseurl, job_id)
    assert not any("ok" in entry for entry in replay["console"])

    wacz = await download_wacz(job_id)
    assert not _wacz_cdx_statuses_for_url(wacz, f"http://{TEST_HOST}/script.js")


@pytest.mark.asyncio
async def test_denylist_blocks_https_subresource_fetch(
    service_client, monitor_client, browser_probe, service_baseurl
):
    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        json={"link": f"https://{TEST_ASSET_HOST}/denylist/asset.css"},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-https-asset-subresource-denylist"
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, service_baseurl, job_id)
    assert any("asset-denylist" in entry for entry in replay["console"])
