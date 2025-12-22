from pathlib import Path

from tests.helpers.s3_bucket import ensure_s3_bucket_exists

secrets_path = Path("secrets/test_secdist.json")
ensure_s3_bucket_exists(secrets_path=secrets_path, endpoint="localhost:8333", bucket="webshot")
