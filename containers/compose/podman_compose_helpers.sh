#!/usr/bin/env bash

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 2; }; }

compose() {
  if podman compose version >/dev/null 2>&1; then
    podman compose "$@"
    return
  fi
  if command -v podman-compose >/dev/null 2>&1; then
    podman-compose "$@"
    return
  fi
  echo "Missing compose support (need 'podman compose' or 'podman-compose')" >&2
  exit 2
}

wait_healthy() {
  local name="$1"
  local timeout_sec="${2:-120}"
  local deadline=$((SECONDS + timeout_sec))

  while true; do
    local status
    status="$(podman inspect -f '{{.State.Health.Status}}' "${name}" 2>/dev/null || true)"

    if [[ "${status}" == "healthy" ]]; then
      return 0
    fi
    if [[ "${status}" == "unhealthy" ]]; then
      echo "Container '${name}' is unhealthy" >&2
      podman logs --tail=200 "${name}" || true
      return 1
    fi

    if ((SECONDS >= deadline)); then
      echo "Timed out waiting for '${name}' to become healthy (status='${status:-unknown}')" >&2
      podman logs --tail=200 "${name}" || true
      return 1
    fi

    sleep 1
  done
}
