#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "${script_dir}/podman_compose_helpers.sh"

need podman
need_compose
need timeout

cd -- "${script_dir}"
compose_file="infra-prodlike.yaml"

timeout_sec="${WEBSHOT_INFRA_DOWN_TIMEOUT_SEC:-90}"
if timeout "${timeout_sec}" compose --in-pod true -f "${compose_file}" down; then
  exit 0
fi

if ! podman pod inspect pod_compose >/dev/null 2>&1; then
  echo "Infra is already down (pod 'pod_compose' not found)." >&2
  exit 0
fi

echo "Warning: podman-compose down failed/timed out after ${timeout_sec}s; forcing pod removal: pod_compose" >&2
podman pod rm -f pod_compose || true
exit 0
