import json
import uuid
from urllib.parse import parse_qs, urlparse
from zipfile import ZIP_STORED

import pytest
from helper.capture_flow import (
    _assert_missing_job_fields,
    _capture_and_wait,
    _probe_replay,
    _wacz_archive_text,
    _wacz_cdx_records,
    _wacz_cdx_statuses_for_url,
    _wacz_entries,
    _wacz_zip_compress_type,
)
from helper.constants import TEST_HOST
from helper.prefix import prefix_key_from_link
from helper.waiters import wait_for_job_status, wait_for_purge


@pytest.mark.asyncio
async def test_capture_and_query_roundtrip(
    service_client,
    pgsql,
    download_wacz,
    browser_probe,
    service_baseurl,
    s3_bucket_name,
):
    link = f"https://{TEST_HOST}/webshot-capture-path"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_body = resp.json()
    uuid_str = job_body["uuid"]
    normalized_link = job_body["link"]
    _assert_missing_job_fields(
        job_body, "started_at", "finished_at", "result_created_at", "result", "error"
    )

    job = await wait_for_job_status(service_client, uuid_str, expected_status="succeeded")
    assert "started_at" in job
    assert "finished_at" in job
    assert "result_created_at" in job
    assert job["result"]["uuid"] == uuid_str
    normalized_link = job["result"]["link"]
    _assert_missing_job_fields(job, "error")

    resp = await service_client.get(f"/v1/capture/{uuid_str}", allow_redirects=False)
    assert resp.status == 200
    capture = resp.json()
    assert capture["uuid"] == uuid_str
    assert capture["link"] == normalized_link
    assert capture["storage_url"].endswith(f"{uuid_str}.wacz")
    assert "Location" not in resp.headers

    resp = await service_client.get(
        f"/v1/capture/{uuid_str}",
        headers={"Host": "webshot.local:8080"},
        allow_redirects=False,
    )
    assert resp.status == 200
    capture_with_host = resp.json()
    assert (
        capture_with_host["storage_url"]
        == f"http://webshot.local:8333/{s3_bucket_name}/{uuid_str}.wacz"
    )

    resp = await service_client.get(
        f"/vendor/replaywebpage/replay/{uuid_str}",
        headers={"Host": "webshot.local:8080"},
        allow_redirects=False,
    )
    assert resp.status == 302
    replay_location = urlparse(resp.headers["Location"])
    replay_source = parse_qs(replay_location.query)["source"][0]
    assert replay_source == f"http://webshot.local:8333/{s3_bucket_name}/{uuid_str}.wacz"

    resp = await service_client.get("/v1/capture", params={"link": normalized_link})
    assert resp.status == 200
    items = resp.json()["items"]
    assert any(item["uuid"] == uuid_str for item in items)

    prefix = urlparse(normalized_link).hostname or TEST_HOST
    resp = await service_client.get("/v1/capture/prefix", params={"prefix": prefix})
    assert resp.status == 200
    prefix_items = resp.json()["items"]
    assert any(
        item["uuid"] == uuid_str and item["link"] == normalized_link for item in prefix_items
    )

    db = pgsql["capture_meta_db"]
    with db.cursor() as cur:
        cur.execute(
            "select prefix_key from capture where id = %s",
            (uuid.UUID(uuid_str),),
        )
        row = cur.fetchone()
    assert row is not None
    (prefix_key,) = row
    assert prefix_key == prefix_key_from_link(normalized_link)

    wacz = await download_wacz(uuid_str)
    entries = _wacz_entries(wacz)
    assert set(entries) == {
        "archive/data.warc.gz",
        "datapackage.json",
        "indexes/index.cdx",
        "logs/stderr.log",
        "logs/stdout.log",
        "pages/pages.jsonl",
    }
    assert _wacz_zip_compress_type(wacz, "archive/data.warc.gz") == ZIP_STORED

    datapackage = json.loads(entries["datapackage.json"].decode("utf-8"))
    assert {resource["path"] for resource in datapackage["resources"]} == {
        "archive/data.warc.gz",
        "indexes/index.cdx",
        "pages/pages.jsonl",
    }
    assert all(resource["hash"].startswith("sha256:") for resource in datapackage["resources"])

    stdout_log = entries["logs/stdout.log"].decode("utf-8")
    assert "browsertrix rewrite start" in stdout_log
    assert "reused_browser=false" in stdout_log
    assert entries["logs/stderr.log"] == b""

    pages_lines = entries["pages/pages.jsonl"].decode("utf-8").splitlines()
    assert len(pages_lines) == 2
    pages_header = json.loads(pages_lines[0])
    seed_page = json.loads(pages_lines[1])
    assert pages_header["format"] == "json-pages-1.0"
    assert pages_header["title"] == "All Pages"
    assert pages_header["hasText"] is False
    assert seed_page["url"] == link
    assert seed_page["title"] == "test"
    assert seed_page["seed"] is True
    assert seed_page["status"] == 200
    assert seed_page["depth"] == 0

    archive_text = _wacz_archive_text(wacz)
    cdx_urls = {record.get("url") for record in _wacz_cdx_records(wacz)}
    assert link in cdx_urls
    assert f"urn:pageinfo:{link}" in cdx_urls
    assert "WARC-Page-ID:" in archive_text
    assert f"WARC-Target-URI: urn:pageinfo:{link}" in archive_text
    assert "WARC-Target-URI: https://test-target/webshot-capture-path" in archive_text
    assert f'"url":"{link}"' in archive_text
    assert f'"{link}":' in archive_text
    assert "http://test-target/webshot-capture-path" not in archive_text
    assert "HTTP/1.1 200 OK" in archive_text
    await _probe_replay(browser_probe, service_baseurl, uuid_str)


@pytest.mark.asyncio
async def test_disallow_and_purge_blocks_new_captures(service_client, monitor_client, pgsql):
    host = TEST_HOST
    link = f"https://{host}/"
    prefix_key = prefix_key_from_link(link)

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    first_job_id = resp.json()["uuid"]
    await wait_for_job_status(service_client, first_job_id, expected_status="succeeded")

    resp = await monitor_client.post("/v1/denylist/disallow_and_purge", json={"link": link})
    assert resp.status == 202

    db = pgsql["capture_meta_db"]
    await wait_for_purge(db, prefix_key)

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 403
    err = resp.json()["error"]["message"]
    assert err == "host in denylist"


@pytest.mark.asyncio
async def test_capture_fails_on_proxy_denied_seed(service_client, pgsql):
    resp = await service_client.post("/v1/capture", json={"link": "http://localhost/"})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    job = await wait_for_job_status(service_client, job_id, expected_status="failed")

    assert "started_at" in job
    assert "finished_at" in job
    _assert_missing_job_fields(job, "result", "result_created_at")
    assert job["error"]["error"]["message"] == "capture failed"
    db = pgsql["capture_meta_db"]
    with db.cursor() as cur:
        cur.execute("select 1 from capture where id = %s", (uuid.UUID(job_id),))
        assert cur.fetchone() is None


@pytest.mark.asyncio
async def test_capture_depth_fetches_additional_resources(
    service_client, browser_probe, download_wacz, service_baseurl
):
    link = f"https://{TEST_HOST}/with-subresource"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, service_baseurl, job_id)
    assert any("ok" in entry for entry in replay["console"])

    wacz = await download_wacz(job_id)
    archive_text = _wacz_archive_text(wacz)
    assert 200 in _wacz_cdx_statuses_for_url(wacz, f"https://{TEST_HOST}/style.css")
    assert 200 in _wacz_cdx_statuses_for_url(wacz, f"https://{TEST_HOST}/script.js")
    assert "WARC-Target-URI: https://test-target/style.css" in archive_text
    assert "WARC-Target-URI: https://test-target/script.js" in archive_text
    assert 'console.log("ok");' in archive_text


@pytest.mark.asyncio
async def test_capture_near_archive_limit_success(
    service_client, download_wacz, test_target_payload_dir
):
    limit_mib = 8
    payload_mib = limit_mib - 3
    mib = 1024 * 1024
    payload_bytes = payload_mib * mib

    import os

    token = uuid.uuid4().hex
    payload_path = test_target_payload_dir / f"{token}.bin"
    try:
        with payload_path.open("wb") as f:
            remaining = payload_bytes
            while remaining:
                chunk = min(remaining, mib)
                f.write(os.urandom(chunk))
                remaining -= chunk

        link = f"https://{TEST_HOST}/near-archive-limit?token={token}"
        job_id, _job = await _capture_and_wait(service_client, link, timeout=40.0)

        wacz = await download_wacz(job_id)
        assert len(wacz) >= payload_mib * mib
        assert len(wacz) < limit_mib * mib
    finally:
        payload_path.unlink(missing_ok=True)


@pytest.mark.asyncio
async def test_capture_records_main_document_redirect_in_wacz(
    service_client, browser_probe, download_wacz, service_baseurl
):
    link = f"https://{TEST_HOST}/redirect-seed"
    job_id, _job = await _capture_and_wait(service_client, link)

    await _probe_replay(browser_probe, service_baseurl, job_id)

    wacz = await download_wacz(job_id)
    archive_text = _wacz_archive_text(wacz)
    assert 302 in _wacz_cdx_statuses_for_url(wacz, f"https://{TEST_HOST}/redirect-seed")
    assert 200 in _wacz_cdx_statuses_for_url(wacz, f"https://{TEST_HOST}/redirect-final")
    assert "HTTP/1.1 302 " in archive_text
    assert "location: /redirect-final" in archive_text
    assert "WARC-Target-URI: https://test-target/redirect-final" in archive_text
    assert "redirect final" in archive_text
