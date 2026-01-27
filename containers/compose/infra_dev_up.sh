#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "${script_dir}/podman_compose_helpers.sh"

need podman
need_compose

cd -- "${script_dir}"
compose_file="infra-dev.yaml"

bash "${script_dir}/ensure_networks.sh"

compose --in-pod true -f "${compose_file}" up -d

wait_healthy egress-proxy 120
wait_healthy servicedb 120
wait_healthy seaweedfs 120

wait_running webshot_scalar 120
wait_running webshot_reverse_proxy 120
wait_running webshot-test-target 120
