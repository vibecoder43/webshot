#!/usr/bin/env bash
set -Eeuo pipefail

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 2; }; }

need bash
need python3

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "${script_dir}/../.." && pwd)"

secdist_path="secret/test_secdist.json"
s3_endpoint_hostport="localhost:8333"
s3_bucket="webshot"
timeout_sec="30"
retry_delay_sec="1"

cd -- "${root}"

if [[ ! -f "${secdist_path}" ]]; then
  echo "Missing secdist file: ${secdist_path}" >&2
  exit 2
fi

python3 -c 'import minio' >/dev/null 2>&1 || {
  echo "Missing Python package: minio" >&2
  exit 2
}
python3 -c 'from tests.helpers.s3_bucket import ensure_s3_bucket_exists' >/dev/null 2>&1 || {
  echo "Failed to import tests.helpers.s3_bucket.ensure_s3_bucket_exists" >&2
  exit 2
}

deadline=$((SECONDS + timeout_sec))
while true; do
  if python3 - <<'PY'
from pathlib import Path

from tests.helpers.s3_bucket import ensure_s3_bucket_exists

ensure_s3_bucket_exists(
    secrets_path=Path("secret/test_secdist.json"),
    endpoint="localhost:8333",
    bucket="webshot",
)
PY
  then
    exit 0
  fi

  if ((SECONDS >= deadline)); then
    echo "Timed out ensuring S3 bucket exists: bucket='${s3_bucket}' endpoint='${s3_endpoint_hostport}'" >&2
    exit 1
  fi
  sleep "${retry_delay_sec}"
done
