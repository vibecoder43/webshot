import asyncio
import io
import json
import uuid
from urllib.parse import urlparse
from zipfile import ZipFile

import pytest
from helpers.constants import TEST_ASSET_HOST, TEST_HOST
from helpers.prefix import prefix_key_from_link
from minio import Minio


async def _wait_for_purge(db, prefix_key: str, timeout: float = 30.0, delay: float = 0.5):
    deadline = asyncio.get_event_loop().time() + timeout
    while True:
        with db.cursor() as cur:
            cur.execute(
                "select count(*) from webshot where prefix_key = %s or prefix_key like %s",
                (prefix_key, f"{prefix_key}/%"),
            )
            (cnt,) = cur.fetchone()
        if cnt == 0:
            return
        if asyncio.get_event_loop().time() >= deadline:
            raise AssertionError(f"purge did not complete; remaining rows: {cnt}")
        await asyncio.sleep(delay)


def _wacz_cdxj_statuses_for_url(wacz: bytes, url: str) -> set[int]:
    statuses: set[int] = set()
    with ZipFile(io.BytesIO(wacz)) as zf:
        cdxj = zf.read("indexes/index.cdxj")
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
        if obj.get("url") != url:
            continue
        status = obj.get("status")
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
async def test_capture_and_query_roundtrip(service_client, pgsql):
    link = f"https://{TEST_HOST}/webshot-capture-path"

    # Create capture
    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 202
    job_body = resp.json()
    uuid_str = job_body["uuid"]
    normalized_link = job_body["link"]

    # Wait for job completion
    for _ in range(120):
        status_resp = await service_client.get(f"/v1/webshot/jobs/{uuid_str}")
        assert status_resp.status == 200
        job = status_resp.json()
        if job["status"] == "succeeded":
            assert job["result"]["uuid"] == uuid_str
            normalized_link = job["result"]["link"]
            break
        if job["status"] == "failed":
            pytest.fail(f"job failed: {job}")
        await asyncio.sleep(0.5)
    else:
        pytest.fail("job did not complete in time")

    # Resolve by id (redirect)
    resp = await service_client.get(f"/v1/webshot/{uuid_str}", allow_redirects=False)
    assert resp.status == 302
    loc = resp.headers.get("Location", "")
    assert loc.endswith(uuid_str)

    # List by exact link
    resp = await service_client.get("/v1/webshot", params={"link": normalized_link})
    assert resp.status == 200
    items = resp.json()["items"]
    assert any(item["uuid"] == uuid_str for item in items)

    # List by prefix (use host prefix)
    prefix = urlparse(normalized_link).hostname or TEST_HOST
    resp = await service_client.get("/v1/webshot/prefix", params={"prefix": prefix})
    assert resp.status == 200
    prefix_items = resp.json()["items"]
    assert any(
        item["uuid"] == uuid_str and item["link"] == normalized_link for item in prefix_items
    )

    # Verify DB row
    db = pgsql["capture_meta_db"]
    with db.cursor() as cur:
        cur.execute(
            "select location, prefix_key from webshot where id = %s",
            (uuid.UUID(uuid_str),),
        )
        row = cur.fetchone()
    assert row is not None
    location, prefix_key = row
    assert location.endswith(uuid_str)
    assert prefix_key == prefix_key_from_link(normalized_link)


@pytest.mark.asyncio
async def test_disallow_and_purge_blocks_new_captures(service_client, pgsql):
    host = TEST_HOST
    link = f"https://{host}/"
    prefix_key = prefix_key_from_link(link)

    # Ensure at least one capture exists before purge
    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 202
    first_job_id = resp.json()["uuid"]
    for _ in range(60):
        status_resp = await service_client.get(f"/v1/webshot/jobs/{first_job_id}")
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
    resp = await service_client.post("/v1/disallow_and_purge", params={"host": link})
    assert resp.status == 202

    # Wait for purge to remove rows for this host
    db = pgsql["capture_meta_db"]
    await _wait_for_purge(db, prefix_key)

    # Attempt new capture should be blocked by denylist
    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 403
    err = resp.json()["error"]["message"]
    assert err == "host in denylist"


@pytest.mark.asyncio
async def test_capture_fails_on_proxy_denied_seed(service_client, pgsql):
    resp = await service_client.post("/v1/webshot", json={"link": "http://localhost/"})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    for _ in range(60):
        status_resp = await service_client.get(f"/v1/webshot/jobs/{job_id}")
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
    with db.cursor() as cur:
        cur.execute("select 1 from webshot where id = %s", (uuid.UUID(job_id),))
        assert cur.fetchone() is None


@pytest.mark.asyncio
async def test_capture_depth_fetches_additional_resources(service_client, service_secdist_path):
    link = f"https://{TEST_HOST}/with-subresource"

    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    for _ in range(120):
        status_resp = await service_client.get(f"/v1/webshot/jobs/{job_id}")
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
    seed_statuses = _wacz_cdxj_statuses_for_url(wacz, f"http://{TEST_HOST}/with-subresource")
    assert 200 in seed_statuses

    style_statuses = _wacz_cdxj_statuses_for_url(wacz, f"http://{TEST_HOST}/style.css")
    assert 200 in style_statuses

    script_statuses = _wacz_cdxj_statuses_for_url(wacz, f"http://{TEST_HOST}/script.js")
    assert 200 in script_statuses


@pytest.mark.asyncio
async def test_denylist_blocks_subresource_fetch(service_client, service_secdist_path):
    deny_resp = await service_client.post(
        "/v1/disallow_and_purge",
        params={"host": f"http://{TEST_HOST}/denylist/style.css"},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-subresource-denylist"
    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    for _ in range(120):
        status_resp = await service_client.get(f"/v1/webshot/jobs/{job_id}")
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
        wacz, f"http://{TEST_HOST}/with-subresource-denylist"
    )
    assert 200 in seed_statuses

    blocked_style_statuses = _wacz_cdxj_statuses_for_url(
        wacz, f"http://{TEST_HOST}/denylist/style.css"
    )
    assert 200 not in blocked_style_statuses

    script_statuses = _wacz_cdxj_statuses_for_url(wacz, f"http://{TEST_HOST}/denylist/script.js")
    assert 200 in script_statuses


@pytest.mark.asyncio
async def test_capture_fetches_https_subresource_assets(service_client, service_secdist_path):
    link = f"https://{TEST_HOST}/with-https-asset-subresource"

    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    for _ in range(120):
        status_resp = await service_client.get(f"/v1/webshot/jobs/{job_id}")
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
        wacz, f"http://{TEST_HOST}/with-https-asset-subresource"
    )
    assert 200 in seed_statuses

    css_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_ASSET_HOST}/asset.css")
    assert 200 in css_statuses

    js_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_ASSET_HOST}/asset.js")
    assert 200 in js_statuses


@pytest.mark.asyncio
async def test_denylist_blocks_https_subresource_fetch(service_client, service_secdist_path):
    deny_resp = await service_client.post(
        "/v1/disallow_and_purge",
        params={"host": f"https://{TEST_ASSET_HOST}/denylist/asset.css"},
    )
    assert deny_resp.status == 202

    link = f"https://{TEST_HOST}/with-https-asset-subresource-denylist"
    resp = await service_client.post("/v1/webshot", json={"link": link})
    assert resp.status == 202
    job_id = resp.json()["uuid"]

    for _ in range(120):
        status_resp = await service_client.get(f"/v1/webshot/jobs/{job_id}")
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
        wacz, f"http://{TEST_HOST}/with-https-asset-subresource-denylist"
    )
    assert 200 in seed_statuses

    blocked_css_statuses = _wacz_cdxj_statuses_for_url(
        wacz, f"https://{TEST_ASSET_HOST}/denylist/asset.css"
    )
    assert 200 not in blocked_css_statuses

    js_statuses = _wacz_cdxj_statuses_for_url(wacz, f"https://{TEST_ASSET_HOST}/denylist/asset.js")
    assert 200 in js_statuses
