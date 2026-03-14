from __future__ import annotations

import json
from pathlib import Path

from s6.common import ToolError, die

_s3_http_timeout_sec = 2.0


def ensure_s3_bucket_exists(*, secrets_path: Path, s3_url: str, bucket: str) -> None:
    if not bucket:
        die("bucket must not be empty", exit_code=2)

    try:
        import minio  # type: ignore[import-not-found]
    except Exception as e:
        raise ToolError(message="Missing Python package: minio", exit_code=2) from e

    try:
        import urllib3  # type: ignore[import-not-found]
    except Exception as e:
        raise ToolError(message="Missing Python package: urllib3", exit_code=2) from e

    try:
        raw = json.loads(secrets_path.read_text(encoding="utf-8"))
    except FileNotFoundError as e:
        raise ToolError(message=f"Missing secdist file: {secrets_path}", exit_code=2) from e

    if not isinstance(raw, dict):
        die("secdist must be a JSON object", exit_code=2)

    creds = raw.get("s3_credentials", {})
    if not isinstance(creds, dict):
        die("secdist s3_credentials must be a JSON object", exit_code=2)

    access_key = creds.get("access_key_id")
    secret_key = creds.get("secret_access_key")
    if not access_key or not secret_key:
        die("secdist must include s3_credentials access_key_id and secret_access_key", exit_code=2)

    http_client = urllib3.PoolManager(
        timeout=urllib3.Timeout(connect=_s3_http_timeout_sec, read=_s3_http_timeout_sec)
    )
    client = minio.Minio(
        s3_url,
        access_key=access_key,
        secret_key=secret_key,
        secure=False,
        http_client=http_client,
    )
    if not client.bucket_exists(bucket):
        client.make_bucket(bucket)
