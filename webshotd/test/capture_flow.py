import asyncio
import gzip
import io
import json
import uuid
from urllib.parse import urlparse
from zipfile import ZIP_STORED, ZipFile

import pytest
from helper.constants import TEST_ASSET_HOST, TEST_HOST
from helper.prefix import prefix_key_from_link
from minio import Minio


def _assert_missing_job_fields(job: dict, *names: str) -> None:
    for name in names:
        assert name not in job


async def _wait_for_purge(db, prefix_key: str, timeout: float = 30.0, delay: float = 0.5):
    deadline = asyncio.get_event_loop().time() + timeout
    while True:
        with db.cursor() as cur:
            cur.execute(
                "select count(*) from capture where prefix_key = %s or prefix_key like %s",
                (prefix_key, f"{prefix_key}/%"),
            )
            (cnt,) = cur.fetchone()
        if cnt == 0:
            return
        if asyncio.get_event_loop().time() >= deadline:
            raise AssertionError(f"purge did not complete; remaining rows: {cnt}")
        await asyncio.sleep(delay)


def _wacz_entries(wacz: bytes) -> dict[str, bytes]:
    with ZipFile(io.BytesIO(wacz)) as zf:
        return {name: zf.read(name) for name in zf.namelist()}


def _wacz_zip_compress_type(wacz: bytes, name: str) -> int:
    with ZipFile(io.BytesIO(wacz)) as zf:
        return zf.getinfo(name).compress_type


def _wacz_cdxj_records(wacz: bytes) -> list[dict[str, object]]:
    entries = _wacz_entries(wacz)
    cdxj = entries["indexes/index.cdxj"]
    records: list[dict[str, object]] = []
    for line in cdxj.splitlines():
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


def _assert_wacz_cdxj_ranges_independently_replayable(wacz: bytes, urls: list[str]) -> None:
    entries = _wacz_entries(wacz)
    warc_gz = entries["archive/data.warc.gz"]

    seen = {url: False for url in urls}

    for record in _wacz_cdxj_records(wacz):
        url = record.get("url")
        if url not in seen:
            continue
        try:
            offset = int(record["offset"])  # type: ignore[arg-type]
            length = int(record["length"])  # type: ignore[arg-type]
        except (KeyError, TypeError, ValueError):
            continue

        assert 0 <= offset < len(warc_gz)
        assert length > 0
        assert offset + length <= len(warc_gz)

        chunk = warc_gz[offset : offset + length]
        assert chunk[:2] == b"\x1f\x8b"
        decompressed = gzip.decompress(chunk)
        assert decompressed.startswith(b"WARC/1.1\r\n")
        seen[str(url)] = True

    missing = [url for url, ok in seen.items() if not ok]
    assert not missing, f"missing independently replayable CDXJ ranges for: {missing!r}"


async def _wait_for_job_status(
    service_client, job_id: str, *, expected_status: str, attempts: int = 120
) -> dict[str, object]:
    for _ in range(attempts):
        status_resp = await service_client.get(f"/v1/capture/jobs/{job_id}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == expected_status:
            return job
        if job["status"] in {"failed", "succeeded"} and job["status"] != expected_status:
            pytest.fail(f"job reached unexpected terminal state: {job}")
        await asyncio.sleep(0.5)
    pytest.fail(f"job did not reach {expected_status!r} in time")


async def _capture_and_wait(
    service_client, link: str, *, expected_status: str = "succeeded", attempts: int = 120
) -> tuple[str, dict[str, object]]:
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]
    return job_id, await _wait_for_job_status(
        service_client,
        job_id,
        expected_status=expected_status,
        attempts=attempts,
    )


def _wacz_cdxj_statuses_for_url(wacz: bytes, url: str) -> set[int]:
    statuses: set[int] = set()
    for record in _wacz_cdxj_records(wacz):
        if record.get("url") != url:
            continue
        status = record.get("status")
        try:
            statuses.add(int(status))
        except (TypeError, ValueError):
            continue
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


@pytest.mark.asyncio
async def test_capture_and_query_roundtrip(service_client, pgsql, service_secdist_path):
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

    # Wait for job completion
    for _ in range(120):
        status_resp = await service_client.get(f"/v1/capture/jobs/{uuid_str}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == "succeeded":
            assert "started_at" in job
            assert "finished_at" in job
            assert "result_created_at" in job
            assert job["result"]["uuid"] == uuid_str
            normalized_link = job["result"]["link"]
            _assert_missing_job_fields(job, "error")
            break
        if job["status"] == "failed":
            pytest.fail(f"job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete in time")

    # Resolve by id (redirect)
    resp = await service_client.get(f"/v1/capture/{uuid_str}", allow_redirects=False)
    assert resp.status == 302
    loc = resp.headers.get("Location", "")
    assert loc.endswith(uuid_str)

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
            "select location, prefix_key from capture where id = %s",
            (uuid.UUID(uuid_str),),
        )
        row = cur.fetchone()
    assert row is not None
    location, prefix_key = row
    assert location.endswith(uuid_str)
    assert prefix_key == prefix_key_from_link(normalized_link)

    wacz = await _download_wacz_from_s3(service_secdist_path, uuid_str)
    entries = _wacz_entries(wacz)
    assert set(entries) == {
        "archive/data.warc.gz",
        "datapackage.json",
        "indexes/index.cdxj",
        "logs/stderr.log",
        "logs/stdout.log",
        "pages/pages.jsonl",
    }
    assert _wacz_zip_compress_type(wacz, "archive/data.warc.gz") == ZIP_STORED

    datapackage = json.loads(entries["datapackage.json"].decode("utf-8"))
    assert any(resource["path"] == "archive/data.warc.gz" for resource in datapackage["resources"])

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
    assert "WARC-Page-ID:" in archive_text
    assert f"WARC-Target-URI: urn:pageinfo:{link}" in archive_text
    assert "WARC-Target-URI: https://test-target/webshot-capture-path" in archive_text
    assert "HTTP/1.1 200 OK" in archive_text
    _assert_wacz_cdxj_ranges_independently_replayable(
        wacz,
        [
            "https://test-target/webshot-capture-path",
            f"urn:pageinfo:{link}",
        ],
    )


@pytest.mark.asyncio
async def test_disallow_and_purge_blocks_new_captures(service_client, pgsql):
    host = TEST_HOST
    link = f"https://{host}/"
    prefix_key = prefix_key_from_link(link)

    # Ensure at least one capture exists before purge
    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    first_job_id = resp.json()["uuid"]
    for _ in range(60):
        status_resp = await service_client.get(f"/v1/capture/jobs/{first_job_id}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == "succeeded":
            break
        if job["status"] == "failed":
            pytest.fail(f"initial job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("initial job did not complete")

    # Disallow and purge
    resp = await service_client.post("/v1/denylist/disallow_and_purge", params={"host": link})
    assert resp.status == 202

    # Wait for purge to remove rows for this host
    db = pgsql["capture_meta_db"]
    await _wait_for_purge(db, prefix_key)

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

    for _ in range(160):
        status_resp = await service_client.get(f"/v1/capture/jobs/{job_id}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == "failed":
            break
        if job["status"] == "succeeded":
            pytest.fail(f"job succeeded unexpectedly: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not fail in time")

    db = pgsql["capture_meta_db"]
    assert "started_at" in job
    assert "finished_at" in job
    _assert_missing_job_fields(job, "result", "result_created_at")
    assert job["error"]["error"]["message"] == "capture failed"
    with db.cursor() as cur:
        cur.execute("select 1 from capture where id = %s", (uuid.UUID(job_id),))
        assert cur.fetchone() is None


@pytest.mark.asyncio
async def test_capture_depth_fetches_additional_resources(service_client, service_secdist_path):
    link = f"https://{TEST_HOST}/with-subresource"

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
            pytest.fail(f"job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete in time")

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    seed_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/with-subresource")
    assert 200 in seed_statuses

    style_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/style.css")
    assert 200 in style_statuses

    script_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/script.js")
    assert 200 in script_statuses


@pytest.mark.asyncio
async def test_capture_near_archive_limit_success(service_client, service_secdist_path):
    limit_mib = 8
    payload_mib = limit_mib - 3
    mib = 1024 * 1024
    payload_bytes = payload_mib * mib

    import os
    import pathlib

    token = uuid.uuid4().hex
    payload_dir = pathlib.Path("/tmp/webshot/testsuite/nginx_payloads")
    payload_dir.mkdir(parents=True, exist_ok=True)
    payload_path = payload_dir / f"{token}.bin"
    try:
        with payload_path.open("wb") as f:
            remaining = payload_bytes
            while remaining:
                chunk = min(remaining, mib)
                f.write(os.urandom(chunk))
                remaining -= chunk

        link = f"https://{TEST_HOST}/near-archive-limit?token={token}"
        job_id, _job = await _capture_and_wait(service_client, link, attempts=240)

        wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
        assert len(wacz) >= payload_mib * mib
        assert len(wacz) < limit_mib * mib
    finally:
        payload_path.unlink(missing_ok=True)


@pytest.mark.asyncio
async def test_capture_records_main_document_redirect_in_wacz(service_client, service_secdist_path):
    link = f"https://{TEST_HOST}/redirect-seed"
    job_id, _job = await _capture_and_wait(service_client, link)

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    redirect_statuses = _wacz_cdxj_statuses_for_url(wacz, link)
    assert 302 in redirect_statuses

    final_url = f"https://{TEST_HOST}/redirect-final"
    final_statuses = _wacz_cdxj_statuses_for_url(wacz, final_url)
    assert 200 in final_statuses

    archive_text = _wacz_archive_text(wacz)
    assert f"WARC-Target-URI: urn:pageinfo:{link}" in archive_text
    assert "WARC-Page-ID:" in archive_text
    assert "WARC-Target-URI: https://test-target/redirect-seed" in archive_text
    assert "location: /redirect-final" in archive_text
    assert "WARC-Target-URI: https://test-target/redirect-final" in archive_text
    assert "Redirect Final" in archive_text


@pytest.mark.asyncio
async def test_capture_preserves_post_subresource_requests(service_client, service_secdist_path):
    link = f"https://{TEST_HOST}/with-post-subresource"
    job_id, _job = await _capture_and_wait(service_client, link)

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    submit_url = f"https://{TEST_HOST}/submit?source=page"
    submit_statuses = _wacz_cdxj_statuses_for_url(wacz, submit_url)
    assert 200 in submit_statuses

    archive_text = _wacz_archive_text(wacz)
    assert "WARC-Target-URI: https://test-target/submit?source=page" in archive_text
    assert "POST /submit?source=page HTTP/1.1" in archive_text


@pytest.mark.asyncio
async def test_capture_preserves_head_subresource_requests(service_client, service_secdist_path):
    link = f"https://{TEST_HOST}/with-head-subresource"
    job_id, _job = await _capture_and_wait(service_client, link)

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    metadata_url = f"https://{TEST_HOST}/metadata?source=page"
    metadata_statuses = _wacz_cdxj_statuses_for_url(wacz, metadata_url)
    assert 200 in metadata_statuses

    archive_text = _wacz_archive_text(wacz)
    assert "WARC-Target-URI: https://test-target/metadata?source=page" in archive_text
    assert "HEAD /metadata?source=page HTTP/1.1" in archive_text


@pytest.mark.asyncio
async def test_capture_preserves_redirected_subresource_hops(service_client, service_secdist_path):
    link = f"https://{TEST_HOST}/with-redirected-asset"
    job_id, _job = await _capture_and_wait(service_client, link)

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    redirect_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/redirect-script.js")
    assert 302 in redirect_statuses

    final_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/script-final.js")
    assert 200 in final_statuses

    archive_text = _wacz_archive_text(wacz)
    assert "WARC-Target-URI: https://test-target/redirect-script.js" in archive_text
    assert "location: /script-final.js" in archive_text
    assert "WARC-Target-URI: https://test-target/script-final.js" in archive_text
    assert "window.__redirectedAssetLoaded = true;" in archive_text


@pytest.mark.asyncio
async def test_denylist_blocks_subresource_fetch(service_client, service_secdist_path):
    deny_resp = await service_client.post(
        "/v1/denylist/disallow_and_purge",
        params={"host": f"https://{TEST_HOST}/denylist/style.css"},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-subresource-denylist"
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
            pytest.fail(f"job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete in time")

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    seed_statuses = _wacz_cdxj_statuses_for_url(
        wacz, f"https://{TEST_HOST}/with-subresource-denylist"
    )
    assert 200 in seed_statuses

    blocked_style_statuses = _wacz_cdxj_statuses_for_url(
        wacz, f"https://{TEST_HOST}/denylist/style.css"
    )
    assert 200 not in blocked_style_statuses

    script_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/denylist/script.js")
    assert 200 in script_statuses


@pytest.mark.asyncio
async def test_capture_fetches_https_subresource_assets(service_client, service_secdist_path):
    link = f"https://{TEST_HOST}/with-https-asset-subresource"

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
            pytest.fail(f"job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete in time")

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    seed_statuses = _wacz_cdxj_statuses_for_url(
        wacz, f"https://{TEST_HOST}/with-https-asset-subresource"
    )
    assert 200 in seed_statuses

    css_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_ASSET_HOST}/asset.css")
    assert 200 in css_statuses

    js_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_ASSET_HOST}/asset.js")
    assert 200 in js_statuses


@pytest.mark.asyncio
async def test_denylist_blocks_https_subresource_fetch(service_client, service_secdist_path):
    deny_resp = await service_client.post(
        "/v1/denylist/disallow_and_purge",
        params={"host": f"https://{TEST_ASSET_HOST}/denylist/asset.css"},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-https-asset-subresource-denylist"
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
            pytest.fail(f"job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete in time")

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    seed_statuses = _wacz_cdxj_statuses_for_url(
        wacz, f"https://{TEST_HOST}/with-https-asset-subresource-denylist"
    )
    assert 200 in seed_statuses

    blocked_css_statuses = _wacz_cdxj_statuses_for_url(
        wacz, f"https://{TEST_ASSET_HOST}/denylist/asset.css"
    )
    assert 200 not in blocked_css_statuses

    js_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_ASSET_HOST}/denylist/asset.js")
    assert 200 in js_statuses


@pytest.mark.asyncio
async def test_https_first_succeeds_when_http_fails(service_client, service_secdist_path):
    link = f"http://{TEST_HOST}/https-first-http-fails"

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
            pytest.fail(f"job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete in time")

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    seed_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_HOST}/https-first-http-fails")
    assert 200 in seed_statuses


@pytest.mark.asyncio
async def test_https_first_falls_back_to_http_when_https_no_response(
    service_client, service_secdist_path
):
    link = f"http://{TEST_HOST}/http-fallback-success"

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
            pytest.fail(f"job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete in time")

    wacz = await _download_wacz_from_s3(service_secdist_path, job_id)
    seed_statuses = _wacz_cdxj_statuses_for_url(wacz, link)
    if 200 not in seed_statuses:
        seed_statuses = _wacz_cdxj_statuses_for_url(wacz, f"{link}/")
    assert 200 in seed_statuses


@pytest.mark.asyncio
async def test_https_first_falls_back_to_http_and_fails_when_http_fails(service_client):
    link = f"http://{TEST_HOST}/http-fallback-fail"

    resp = await service_client.post("/v1/capture", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    for _ in range(160):
        status_resp = await service_client.get(f"/v1/capture/jobs/{job_id}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == "failed":
            break
        if job["status"] == "succeeded":
            pytest.fail(f"job succeeded unexpectedly: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not fail in time")


@pytest.mark.asyncio
async def test_sequential_captures_do_not_reuse_browser_state(service_client, service_secdist_path):
    first_job_id, _first_job = await _capture_and_wait(
        service_client, f"https://{TEST_HOST}/state-write"
    )
    second_job_id, _second_job = await _capture_and_wait(
        service_client, f"https://{TEST_HOST}/state-read"
    )

    assert first_job_id != second_job_id

    wacz = await _download_wacz_from_s3(service_secdist_path, second_job_id)
    archive_text = _wacz_archive_text(wacz)
    assert "seen=first" not in archive_text
    assert '"localStorage":"first"' not in archive_text
    assert '"sessionStorage":"first"' not in archive_text
    assert '"localStorage":null' in archive_text
    assert '"sessionStorage":null' in archive_text
