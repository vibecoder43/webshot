import asyncio
import gzip
import io
import json
import uuid
from urllib.parse import parse_qs, urlparse
from zipfile import ZIP_STORED, ZipFile

import pytest
from helper.constants import TEST_ASSET_HOST, TEST_HOST, UNTRUSTED_TEST_HOST
from helper.prefix import prefix_key_from_link
from helper.waiters import wait_for_job_status, wait_for_purge
from minio import Minio


def _enable_allowlist_only(_config_yaml, config_vars):
    config_vars["allowlist_only"] = True


def _enable_https_only(_config_yaml, config_vars):
    config_vars["https_only"] = True


def _assert_missing_job_fields(job: dict, *names: str) -> None:
    for name in names:
        assert name not in job


def _wacz_entries(wacz: bytes) -> dict[str, bytes]:
    with ZipFile(io.BytesIO(wacz)) as zf:
        return {name: zf.read(name) for name in zf.namelist()}


def _wacz_object_name(capture_uuid: str) -> str:
    return f"{capture_uuid}.wacz"


def _wacz_zip_compress_type(wacz: bytes, name: str) -> int:
    with ZipFile(io.BytesIO(wacz)) as zf:
        return zf.getinfo(name).compress_type


def _wacz_cdx_records(wacz: bytes) -> list[dict[str, object]]:
    entries = _wacz_entries(wacz)
    cdx = entries["indexes/index.cdx"]
    records: list[dict[str, object]] = []
    for line in cdx.splitlines():
        if not line:
            continue
        json_pos = line.find(b"{")
        if json_pos == -1:
            continue
        try:
            obj = json.loads(line[json_pos:].decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            continue
        prefix = line[:json_pos].decode("utf-8").strip()
        try:
            key, timestamp = prefix.split(" ", 1)
        except ValueError:
            continue
        record = dict(obj)
        record["key"] = key
        record["timestamp"] = timestamp
        records.append(record)
    return records


def _wacz_archive_text(wacz: bytes) -> str:
    gz = _wacz_entries(wacz)["archive/data.warc.gz"]
    return gzip.decompress(gz).decode("utf-8", "replace")


async def _capture_and_wait(
    service_client,
    link: str,
    *,
    expected_status: str = "succeeded",
    timeout: float | None = None,
) -> tuple[str, dict[str, object]]:
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]
    if timeout is None:
        job = await wait_for_job_status(service_client, job_id, expected_status=expected_status)
    else:
        job = await wait_for_job_status(
            service_client, job_id, expected_status=expected_status, timeout=timeout
        )
    return job_id, job


def _wacz_cdxj_statuses_for_url(wacz: bytes, url: str) -> set[int]:
    statuses: set[int] = set()
    for record in _wacz_cdx_records(wacz):
        if record.get("url") != url:
            continue
        status = record.get("status")
        try:
            statuses.add(int(status))
        except (TypeError, ValueError):
            continue
    return statuses


def _root_url_variants(url: str) -> set[str]:
    parsed = urlparse(url)
    if parsed.path not in ("", "/") or parsed.params or parsed.query or parsed.fragment:
        return {url}
    base = f"{parsed.scheme}://{parsed.netloc}"
    return {base, f"{base}/"}


def _wacz_cdxj_statuses_for_root_url(wacz: bytes, url: str) -> set[int]:
    statuses: set[int] = set()
    for variant in _root_url_variants(url):
        statuses.update(_wacz_cdxj_statuses_for_url(wacz, variant))
    return statuses


async def _download_wacz_from_s3(service_secdist_path, object_name: str) -> bytes:
    def _get() -> bytes:
        import json

        with service_secdist_path.open(encoding="utf-8") as f:
            raw = json.load(f)
        creds = raw["s3_credentials"]
        client = Minio(
            "localhost:8333",
            access_key=creds["access_key_id"],
            secret_key=creds["secret_access_key"],
            secure=False,
        )
        resp = client.get_object("webshot", object_name)
        try:
            return resp.read()
        finally:
            resp.close()
            resp.release_conn()

    return await asyncio.to_thread(_get)


@pytest.fixture
def download_wacz(service_secdist_path):
    async def _download(capture_uuid: str) -> bytes:
        return await _download_wacz_from_s3(service_secdist_path, _wacz_object_name(capture_uuid))

    return _download


def _replay_page_url(capture_uuid: str) -> str:
    return f"http://127.0.0.1:8080/vendor/replaywebpage/replay/{capture_uuid}"


def _replay_wait_expression() -> str:
    return """
(() => {
  const app = document.querySelector("replay-app-main");
  const item = app?.shadowRoot?.querySelector("wr-item");
  const replay = item?.shadowRoot?.querySelector("wr-coll-replay");
  const iframe = replay?.shadowRoot?.querySelector("iframe");
  const rendered = iframe?.contentDocument;
  return !!app
    && !!replay
    && !!iframe
    && iframe.src.includes("/vendor/replaywebpage/w/")
    && rendered?.readyState === "complete"
    && !!rendered.body;
})()
""".strip()


async def _probe_replay(browser_probe, capture_uuid: str) -> dict:
    replay = await browser_probe(
        _replay_page_url(capture_uuid),
        wait_expression=_replay_wait_expression(),
        timeout_ms=15_000,
    )
    assert replay["page_errors"] == []
    return replay


@pytest.mark.asyncio
async def test_capture_and_query_roundtrip(service_client, pgsql, download_wacz, browser_probe):
    link = f"https://{TEST_HOST}/webshot-capture-path"

    # Create capture
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

    # Resolve by id
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
    assert capture_with_host["storage_url"] == f"http://webshot.local:8333/webshot/{uuid_str}.wacz"

    resp = await service_client.get(
        f"/vendor/replaywebpage/replay/{uuid_str}",
        headers={"Host": "webshot.local:8080"},
        allow_redirects=False,
    )
    assert resp.status == 302
    replay_location = urlparse(resp.headers["Location"])
    replay_source = parse_qs(replay_location.query)["source"][0]
    assert replay_source == f"http://webshot.local:8333/webshot/{uuid_str}.wacz"

    # List by exact link
    resp = await service_client.get("/v1/capture", params={"link": normalized_link})
    assert resp.status == 200
    items = resp.json()["items"]
    assert any(item["uuid"] == uuid_str for item in items)

    # List by prefix (use host prefix)
    prefix = urlparse(normalized_link).hostname or TEST_HOST
    resp = await service_client.get("/v1/capture/prefix", params={"prefix": prefix})
    assert resp.status == 200
    prefix_items = resp.json()["items"]
    assert any(
        item["uuid"] == uuid_str and item["link"] == normalized_link for item in prefix_items
    )

    # Verify DB row
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
    await _probe_replay(browser_probe, uuid_str)


@pytest.mark.asyncio
async def test_disallow_and_purge_blocks_new_captures(service_client, monitor_client, pgsql):
    host = TEST_HOST
    link = f"https://{host}/"
    prefix_key = prefix_key_from_link(link)

    # Ensure at least one capture exists before purge
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    first_job_id = resp.json()["uuid"]
    await wait_for_job_status(service_client, first_job_id, expected_status="succeeded")

    # Disallow and purge
    resp = await monitor_client.post("/v1/denylist/disallow_and_purge", params={"host": link})
    assert resp.status == 202

    # Wait for purge to remove rows for this host
    db = pgsql["capture_meta_db"]
    await wait_for_purge(db, prefix_key)

    # Attempt new capture should be blocked by denylist
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
    service_client, browser_probe, download_wacz
):
    link = f"https://{TEST_HOST}/with-subresource"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, job_id)
    assert any("ok" in entry for entry in replay["console"])

    wacz = await download_wacz(job_id)
    archive_text = _wacz_archive_text(wacz)
    assert 200 in _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/style.css")
    assert 200 in _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/script.js")
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
    service_client, browser_probe, download_wacz
):
    link = f"https://{TEST_HOST}/redirect-seed"
    job_id, _job = await _capture_and_wait(service_client, link)

    await _probe_replay(browser_probe, job_id)

    wacz = await download_wacz(job_id)
    archive_text = _wacz_archive_text(wacz)
    assert 302 in _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/redirect-seed")
    assert 200 in _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/redirect-final")
    assert "HTTP/1.1 302 " in archive_text
    assert "location: /redirect-final" in archive_text
    assert "WARC-Target-URI: https://test-target/redirect-final" in archive_text
    assert "redirect final" in archive_text


@pytest.mark.asyncio
async def test_capture_preserves_post_subresource_requests(service_client, download_wacz):
    link = f"https://{TEST_HOST}/with-post-subresource"
    job_id, _job = await _capture_and_wait(service_client, link)

    wacz = await download_wacz(job_id)
    submit_url = f"https://{TEST_HOST}/submit?source=page"
    submit_statuses = _wacz_cdxj_statuses_for_url(wacz, submit_url)
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
    metadata_statuses = _wacz_cdxj_statuses_for_url(wacz, metadata_url)
    assert 200 in metadata_statuses

    archive_text = _wacz_archive_text(wacz)
    assert "WARC-Target-URI: https://test-target/metadata?source=page" in archive_text
    assert "HEAD /metadata?source=page HTTP/1.1" in archive_text


@pytest.mark.asyncio
async def test_capture_preserves_redirected_subresource_hops(
    service_client, browser_probe, download_wacz
):
    link = f"https://{TEST_HOST}/with-redirected-asset"
    job_id, _job = await _capture_and_wait(service_client, link)

    await _probe_replay(browser_probe, job_id)

    wacz = await download_wacz(job_id)
    archive_text = _wacz_archive_text(wacz)
    assert 302 in _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/redirect-script.js")
    assert 200 in _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/script-final.js")
    assert "location: /script-final.js" in archive_text
    assert "window.__redirectedAssetLoaded = true;" in archive_text


@pytest.mark.asyncio
async def test_denylist_blocks_subresource_fetch(
    service_client, monitor_client, browser_probe, download_wacz
):
    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        params={"host": f"https://{TEST_HOST}/denylist/style.css"},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-subresource-denylist"
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, job_id)
    assert any("denylist" in entry for entry in replay["console"])


@pytest.mark.asyncio
async def test_regular_mode_allowlist_overrides_denylisted_subresource_fetch(
    service_client, monitor_client, browser_probe
):
    script = f"https://{TEST_HOST}/denylist/script.js"

    allow_resp = await monitor_client.post("/v1/allowlist/add", params={"link": script})
    assert allow_resp.status == 204

    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        params={"host": script},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-subresource-denylist"
    job_id, _job = await _capture_and_wait(service_client, link)

    replay = await _probe_replay(browser_probe, job_id)
    assert any("denylist" in entry for entry in replay["console"])


@pytest.mark.uservice_oneshot(config_hooks=[_enable_allowlist_only])
@pytest.mark.asyncio
async def test_allowlist_only_fetches_allowlisted_subresources(
    service_client, monitor_client, browser_probe, download_wacz
):
    seed = f"https://{TEST_HOST}/with-subresource"
    style = f"https://{TEST_HOST}/style.css"
    script = f"https://{TEST_HOST}/script.js"
    for link in [seed, style, script]:
        resp = await monitor_client.post("/v1/allowlist/add", params={"link": link})
        assert resp.status == 204

    job_id, _job = await _capture_and_wait(service_client, seed)

    replay = await _probe_replay(browser_probe, job_id)
    assert any("ok" in entry for entry in replay["console"])

    wacz = await download_wacz(job_id)
    assert 200 in _wacz_cdxj_statuses_for_url(wacz, style)
    assert 200 in _wacz_cdxj_statuses_for_url(wacz, script)


@pytest.mark.uservice_oneshot(config_hooks=[_enable_allowlist_only])
@pytest.mark.asyncio
async def test_allowlist_only_blocks_non_allowlisted_subresources(
    service_client, monitor_client, browser_probe, download_wacz
):
    seed = f"https://{TEST_HOST}/with-https-asset-subresource"
    asset_script = f"https://{TEST_ASSET_HOST}/asset.js"

    resp = await monitor_client.post("/v1/allowlist/add", params={"link": seed})
    assert resp.status == 204

    job_id, _job = await _capture_and_wait(service_client, seed)

    replay = await _probe_replay(browser_probe, job_id)
    assert not any("asset" in entry for entry in replay["console"])

    wacz = await download_wacz(job_id)
    assert 403 in _wacz_cdxj_statuses_for_url(wacz, asset_script)


@pytest.mark.asyncio
async def test_capture_fetches_https_subresource_assets(
    service_client, browser_probe, download_wacz
):
    link = f"https://{TEST_HOST}/with-https-asset-subresource"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, job_id)
    assert any("asset" in entry for entry in replay["console"])


@pytest.mark.uservice_oneshot(config_hooks=[_enable_https_only])
@pytest.mark.asyncio
async def test_https_only_blocks_http_subresource_fetch(
    service_client, browser_probe, download_wacz
):
    link = f"https://{TEST_HOST}/with-http-subresource"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, job_id)
    assert not any("ok" in entry for entry in replay["console"])

    wacz = await download_wacz(job_id)
    assert not _wacz_cdxj_statuses_for_url(wacz, f"http://{TEST_HOST}/script.js")


@pytest.mark.asyncio
async def test_denylist_blocks_https_subresource_fetch(
    service_client, monitor_client, browser_probe, download_wacz
):
    deny_resp = await monitor_client.post(
        "/v1/denylist/disallow_and_purge",
        params={"host": f"https://{TEST_ASSET_HOST}/denylist/asset.css"},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-https-asset-subresource-denylist"
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    replay = await _probe_replay(browser_probe, job_id)
    assert any("asset-denylist" in entry for entry in replay["console"])


@pytest.mark.uservice_oneshot(config_hooks=[_enable_allowlist_only])
@pytest.mark.asyncio
async def test_allowlist_only_blocks_non_allowlisted_redirect_target(
    service_client, monitor_client
):
    seed = f"https://{TEST_HOST}/redirect-seed"

    resp = await monitor_client.post("/v1/allowlist/add", params={"link": seed})
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
async def test_https_first_succeeds_when_http_fails(service_client, browser_probe, download_wacz):
    link = f"http://{TEST_HOST}/https-first-http-fails"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    await _probe_replay(browser_probe, job_id)

    wacz = await download_wacz(job_id)
    assert 200 in _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/https-first-http-fails")
    assert not _wacz_cdxj_statuses_for_url(wacz, f"http://{TEST_HOST}/https-first-http-fails")


@pytest.mark.uservice_oneshot(config_hooks=[_enable_https_only])
@pytest.mark.asyncio
async def test_https_only_accepts_http_seed_and_crawls_https(
    service_client, browser_probe, download_wacz
):
    link = f"http://{TEST_HOST}/https-first-http-fails"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    await _probe_replay(browser_probe, job_id)

    wacz = await download_wacz(job_id)
    assert 200 in _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/https-first-http-fails")
    assert not _wacz_cdxj_statuses_for_url(wacz, f"http://{TEST_HOST}/https-first-http-fails")


@pytest.mark.asyncio
async def test_https_first_falls_back_to_http_when_https_no_response(
    service_client, browser_probe, download_wacz
):
    link = f"http://{TEST_HOST}/http-fallback-success"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    await wait_for_job_status(service_client, job_id, expected_status="succeeded")

    await _probe_replay(browser_probe, job_id)

    wacz = await download_wacz(job_id)
    assert 200 in _wacz_cdxj_statuses_for_url(wacz, f"http://{TEST_HOST}/http-fallback-success")
    assert not _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/http-fallback-success")


@pytest.mark.uservice_oneshot(config_hooks=[_enable_https_only])
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
    service_client, browser_probe, download_wacz
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

    await _probe_replay(browser_probe, job_id)

    wacz = await download_wacz(job_id)
    archive_text = _wacz_archive_text(wacz)
    assert 200 in _wacz_cdxj_statuses_for_root_url(wacz, f"http://{UNTRUSTED_TEST_HOST}/")
    assert not _wacz_cdxj_statuses_for_root_url(wacz, f"https://{UNTRUSTED_TEST_HOST}/")
    assert "WARC-Target-URI: http://untrusted.test-target/" in archive_text
    assert "untrusted" in archive_text


@pytest.mark.asyncio
async def test_sequential_captures_do_not_reuse_browser_state(
    service_client, browser_probe, download_wacz
):
    first_job_id, _first_job = await _capture_and_wait(
        service_client, f"https://{TEST_HOST}/state-write"
    )
    second_job_id, _second_job = await _capture_and_wait(
        service_client, f"https://{TEST_HOST}/state-read"
    )

    assert first_job_id != second_job_id

    await _probe_replay(browser_probe, second_job_id)

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
