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

compose_file="infra_prodlike.yaml"

infra_down_compose "${script_dir}" "${compose_file}"
exit 0
