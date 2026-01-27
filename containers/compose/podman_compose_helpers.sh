#!/usr/bin/env bash

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 2; }; }

need_compose() {
  if podman compose version >/dev/null 2>&1; then
    return 0
  fi
  if command -v podman-compose >/dev/null 2>&1; then
    return 0
  fi
  echo "Missing compose support (need 'podman compose' or 'podman-compose')" >&2
  exit 2
}

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

podman_inspect_state() {
  local name="$1"
  local out
  local err
  err="$(mktemp)"
  if ! out="$(podman inspect -f '{{.State.Status}}|{{if .State.Health}}{{.State.Health.Status}}{{end}}|{{.State.ExitCode}}' "${name}" 2>"${err}")"; then
    PODMAN_LAST_ERROR="$(<"${err}")"
    rm -f "${err}"
    return 1
  fi
  rm -f "${err}"
  PODMAN_LAST_ERROR=""
  printf '%s' "${out}"
}

wait_healthy() {
  local name="$1"
  local timeout_sec="${2:-120}"
  local deadline=$((SECONDS + timeout_sec))
  local last_reported=""
  local inspect_errors=0

  while true; do
    local state health exit_code
    local status_line
    if ! status_line="$(podman_inspect_state "${name}")"; then
      inspect_errors=$((inspect_errors + 1))
      if ((inspect_errors >= 5)); then
        echo "Failed to inspect container '${name}' via podman after ${inspect_errors} attempts." >&2
        [[ -n "${PODMAN_LAST_ERROR:-}" ]] && echo "${PODMAN_LAST_ERROR}" >&2
        return 1
      fi
      sleep 1
      continue
    fi
    inspect_errors=0
    IFS='|' read -r state health exit_code <<<"${status_line}"

    if [[ "${state}" != "running" && -n "${state}" ]]; then
      echo "Container '${name}' is not running (state='${state}', exit_code='${exit_code}')." >&2
      podman logs --tail=200 "${name}" || true
      return 1
    fi

    # In some environments (e.g. rootless Podman without a systemd user session),
    # Podman cannot schedule healthcheck timers. We rely on `healthcheck.interval`
    # being set to "disable" in compose, and drive checks manually here.
    podman healthcheck run "${name}" >/dev/null 2>&1 || true

    # Refresh health after running the healthcheck once.
    if ! status_line="$(podman_inspect_state "${name}")"; then
      inspect_errors=$((inspect_errors + 1))
      if ((inspect_errors >= 5)); then
        echo "Failed to inspect container '${name}' via podman after ${inspect_errors} attempts." >&2
        [[ -n "${PODMAN_LAST_ERROR:-}" ]] && echo "${PODMAN_LAST_ERROR}" >&2
        return 1
      fi
      sleep 1
      continue
    fi
    inspect_errors=0
    IFS='|' read -r state health exit_code <<<"${status_line}"

    if [[ "${health}" == "healthy" ]]; then
      return 0
    fi
    if [[ "${health}" == "unhealthy" ]]; then
      echo "Container '${name}' is unhealthy" >&2
      podman logs --tail=200 "${name}" || true
      return 1
    fi

    if ((SECONDS >= deadline)); then
      echo "Timed out waiting for '${name}' to become healthy (health='${health:-unknown}', state='${state:-unknown}')" >&2
      podman logs --tail=200 "${name}" || true
      return 1
    fi

    local report="state='${state:-unknown}' health='${health:-unknown}'"
    if [[ "${report}" != "${last_reported}" ]]; then
      echo "Waiting for '${name}' to become healthy (${report})" >&2
      last_reported="${report}"
    fi

    sleep 1
  done
}

wait_running() {
  local name="$1"
  local timeout_sec="${2:-120}"
  local deadline=$((SECONDS + timeout_sec))
  local last_reported=""
  local inspect_errors=0

  while true; do
    local state health exit_code
    local status_line
    if ! status_line="$(podman_inspect_state "${name}")"; then
      inspect_errors=$((inspect_errors + 1))
      if ((inspect_errors >= 5)); then
        echo "Failed to inspect container '${name}' via podman after ${inspect_errors} attempts." >&2
        [[ -n "${PODMAN_LAST_ERROR:-}" ]] && echo "${PODMAN_LAST_ERROR}" >&2
        return 1
      fi
      sleep 1
      continue
    fi
    inspect_errors=0
    IFS='|' read -r state health exit_code <<<"${status_line}"

    if [[ "${state}" == "running" ]]; then
      return 0
    fi
    if [[ "${state}" == "exited" || "${state}" == "stopped" || "${state}" == "dead" ]]; then
      echo "Container '${name}' is not running (state='${state}', exit_code='${exit_code}')." >&2
      podman logs --tail=200 "${name}" 2>/dev/null || true
      return 1
    fi

    if ((SECONDS >= deadline)); then
      echo "Timed out waiting for '${name}' to be running (state='${state:-unknown}')" >&2
      podman ps -a --filter "name=^${name}$" --format '{{.Names}} {{.Status}}' 2>/dev/null || true
      podman logs --tail=200 "${name}" 2>/dev/null || true
      return 1
    fi

    local report="state='${state:-unknown}'"
    if [[ "${report}" != "${last_reported}" ]]; then
      echo "Waiting for '${name}' to be running (${report})" >&2
      last_reported="${report}"
    fi

    sleep 1
  done
}
