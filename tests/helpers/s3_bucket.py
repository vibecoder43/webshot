import json
import pathlib

import minio


def ensure_s3_bucket_exists(secrets_path: pathlib.Path, endpoint: str, bucket: str) -> None:
    if not bucket:
        raise ValueError("bucket must not be empty")

    with secrets_path.open(encoding="utf-8") as f:
        raw = json.load(f)
    if not isinstance(raw, dict):
        raise ValueError("secdist must be a JSON object")

    creds = raw.get("s3_credentials", {})
    if not isinstance(creds, dict):
        raise ValueError("secdist s3_credentials must be a JSON object")

    access_key = creds.get("access_key_id")
    secret_key = creds.get("secret_access_key")
    if not access_key or not secret_key:
        raise ValueError("secdist must include s3_credentials access_key_id and secret_access_key")

    client = minio.Minio(
        endpoint,
        access_key=access_key,
        secret_key=secret_key,
        secure=False,
    )
    if not client.bucket_exists(bucket):
        client.make_bucket(bucket)
