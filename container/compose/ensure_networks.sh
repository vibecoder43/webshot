#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=container/compose/podman_compose_helpers.sh
. "${script_dir}/podman_compose_helpers.sh"

need podman

ensure_network() {
  local name="$1"
  local want_internal="$2"

  if podman network inspect "${name}" >/dev/null 2>&1; then
    local internal
    internal="$(podman network inspect -f '{{.Internal}}' "${name}" 2>/dev/null || true)"
    if [[ "${internal}" != "${want_internal}" ]]; then
      echo "Podman network '${name}' has Internal='${internal}', expected '${want_internal}'." >&2
      echo "Bring down containers that use it and recreate it:" >&2
      echo "  podman network rm '${name}'" >&2
      if [[ "${want_internal}" == "true" ]]; then
        echo "  podman network create --internal '${name}'" >&2
      else
        echo "  podman network create '${name}'" >&2
      fi
      exit 1
    fi
    return 0
  fi

  if [[ "${want_internal}" == "true" ]]; then
    podman network create --internal "${name}" >/dev/null
  else
    podman network create "${name}" >/dev/null
  fi
}

ensure_network crawler_net true
ensure_network egress_net false
