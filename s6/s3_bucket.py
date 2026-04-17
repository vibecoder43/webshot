from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path

from minio import Minio
from minio.helpers import DictType

from s6.common import ToolError, die

_s3_http_timeout_sec = 2.0
_bucket_cors_xml = b"""<?xml version="1.0" encoding="UTF-8"?>
<CORSConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <CORSRule>
    <AllowedOrigin>*</AllowedOrigin>
    <AllowedMethod>GET</AllowedMethod>
    <AllowedMethod>HEAD</AllowedMethod>
    <AllowedHeader>*</AllowedHeader>
    <ExposeHeader>Accept-Ranges</ExposeHeader>
    <ExposeHeader>Content-Range</ExposeHeader>
    <ExposeHeader>Content-Length</ExposeHeader>
    <ExposeHeader>Content-Type</ExposeHeader>
    <ExposeHeader>ETag</ExposeHeader>
    <ExposeHeader>Last-Modified</ExposeHeader>
    <ExposeHeader>Content-Disposition</ExposeHeader>
    <MaxAgeSeconds>3600</MaxAgeSeconds>
  </CORSRule>
</CORSConfiguration>
"""


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
    _ensure_bucket_cors(client, bucket)


def _ensure_bucket_cors(client: Minio, bucket: str) -> None:
    content_md5 = base64.b64encode(hashlib.md5(_bucket_cors_xml).digest()).decode("ascii")
    headers: DictType = {
        "Content-Type": "application/xml",
        "Content-MD5": content_md5,
    }
    query_params: DictType = {"cors": ""}
    client._execute(
        "PUT",
        bucket_name=bucket,
        body=_bucket_cors_xml,
        headers=headers,
        query_params=query_params,
    )
