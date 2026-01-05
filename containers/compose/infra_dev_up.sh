#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "${script_dir}/podman_compose_helpers.sh"

need podman
need podman-compose

cd -- "${script_dir}"
compose_file="infra-dev.yaml"

bash "${script_dir}/ensure_networks.sh"

podman-compose --in-pod true -f "${compose_file}" up -d

wait_healthy egress-proxy 120
wait_healthy servicedb 120
wait_healthy seaweedfs 120

wait_running() {
  local name="$1"
  local timeout_sec="${2:-120}"
  local deadline=$((SECONDS + timeout_sec))

  while true; do
    local status
    status="$(podman inspect -f '{{.State.Status}}' "${name}" 2>/dev/null || true)"
    if [[ "${status}" == "running" ]]; then
      return 0
    fi
    if ((SECONDS >= deadline)); then
      echo "Timed out waiting for '${name}' to be running (status='${status:-missing}')" >&2
      podman ps -a --filter "name=^${name}$" --format '{{.Names}} {{.Status}}' 2>/dev/null || true
      podman logs --tail=200 "${name}" 2>/dev/null || true
      return 1
    fi
    sleep 1
  done
}

wait_running webshot_scalar 120
wait_running webshot_reverse_proxy 120
wait_running webshot-test-target 120
