#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=container/compose/podman_compose_helpers.sh
. "${script_dir}/podman_compose_helpers.sh"
# shellcheck source=container/compose/infra_helpers.sh
. "${script_dir}/infra_helpers.sh"

need podman
need_compose
need timeout

infra_mitm_bootstrap_if_needed "${script_dir}" dev

cd -- "${script_dir}"
compose_file="infra_dev.yaml"

bash "${script_dir}/ensure_networks.sh"

compose --in-pod true -f "${compose_file}" up -d

wait_healthy egress_proxy 120
wait_healthy servicedb 120
ensure_servicedb_timezone_utc_or_down "${script_dir}" "${compose_file}" servicedb || exit 1
wait_healthy seaweedfs 120

bash "${script_dir}/ensure_s3_bucket_dev.sh"

wait_running webshot_scalar 120
wait_running webshot_reverse_proxy 120
wait_running webshot_test_target 120
