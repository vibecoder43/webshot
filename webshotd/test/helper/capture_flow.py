import asyncio
import gzip
import io
import json
from urllib.parse import urljoin, urlparse
from zipfile import ZipFile

import pytest
from helper.config_hooks import enable_allowlist_only, enable_https_only
from helper.waiters import wait_for_job_status
from minio import Minio

_enable_allowlist_only = enable_allowlist_only


_enable_https_only = enable_https_only


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


def _wacz_cdx_statuses_for_url(wacz: bytes, url: str) -> set[int]:
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


def _wacz_cdx_statuses_for_root_url(wacz: bytes, url: str) -> set[int]:
    statuses: set[int] = set()
    for variant in _root_url_variants(url):
        statuses.update(_wacz_cdx_statuses_for_url(wacz, variant))
    return statuses


async def _download_wacz_from_s3(service_secdist_path, bucket_name: str, object_name: str) -> bytes:
    def _get() -> bytes:
        import json

        with service_secdist_path.open(encoding="utf-8") as f:
            raw = json.load(f)
        creds = raw["s3_credentials"]
        client = Minio(
            "127.0.0.1:8333",
            access_key=creds["access_key_id"],
            secret_key=creds["secret_access_key"],
            secure=False,
        )
        resp = client.get_object(bucket_name, object_name)
        try:
            return resp.read()
        finally:
            resp.close()
            resp.release_conn()

    return await asyncio.to_thread(_get)


@pytest.fixture
def download_wacz(service_secdist_path, s3_bucket_name: str):
    async def _download(capture_uuid: str) -> bytes:
        return await _download_wacz_from_s3(
            service_secdist_path,
            s3_bucket_name,
            _wacz_object_name(capture_uuid),
        )

    return _download


def _replay_page_url(service_baseurl: str, capture_uuid: str) -> str:
    return urljoin(service_baseurl, f"vendor/replaywebpage/replay/{capture_uuid}")


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


async def _probe_replay(browser_probe, service_baseurl: str, capture_uuid: str) -> dict:
    replay = await browser_probe(
        _replay_page_url(service_baseurl, capture_uuid),
        wait_expression=_replay_wait_expression(),
        timeout_ms=15_000,
    )
    assert replay["page_errors"] == []
    return replay
