#!/usr/bin/env bash

assert_servicedb_timezone_utc() {
  local container_name="${1:-servicedb}"

  local tz=""
  local log_tz=""
  local tz_raw=""
  local log_tz_raw=""
  if ! tz_raw="$(podman exec "${container_name}" psql -U postgres -tAqc 'SHOW TimeZone;' 2>&1)"; then
    echo "Failed to read Postgres TimeZone from container '${container_name}':" >&2
    echo "${tz_raw}" >&2
    return 1
  fi
  if ! log_tz_raw="$(podman exec "${container_name}" psql -U postgres -tAqc 'SHOW log_timezone;' 2>&1)"; then
    echo "Failed to read Postgres log_timezone from container '${container_name}':" >&2
    echo "${log_tz_raw}" >&2
    return 1
  fi

  tz="$(printf '%s' "${tz_raw}" | tr -d '[:space:]')"
  log_tz="$(printf '%s' "${log_tz_raw}" | tr -d '[:space:]')"

  if [[ -z "${tz}" || -z "${log_tz}" ]]; then
    echo "Failed to read Postgres timezone settings from container '${container_name}'." >&2
    return 1
  fi

  if [[ "${tz}" != "UTC" ]]; then
    echo "Postgres TimeZone must be UTC, got '${tz}' (container='${container_name}')." >&2
    return 1
  fi
  if [[ "${log_tz}" != "UTC" ]]; then
    echo "Postgres log_timezone must be UTC, got '${log_tz}' (container='${container_name}')." >&2
    return 1
  fi

  return 0
}

ensure_servicedb_timezone_utc_or_down() {
  local script_dir="$1"
  local compose_file="$2"
  local container_name="${3:-servicedb}"

  if assert_servicedb_timezone_utc "${container_name}"; then
    return 0
  fi

  infra_down_compose "${script_dir}" "${compose_file}" || true
  return 1
}

infra_down_compose() {
  local script_dir="$1"
  local compose_file="$2"

  cd -- "${script_dir}" || return 1

  local timeout_sec="${WEBSHOT_INFRA_DOWN_TIMEOUT_SEC:-90}"
  if timeout "${timeout_sec}" compose --in-pod true -f "${compose_file}" down; then
    return 0
  fi

  if ! podman pod inspect pod_compose >/dev/null 2>&1; then
    echo "Infra is already down (pod 'pod_compose' not found)." >&2
    return 0
  fi

  echo "Warning: podman-compose down failed/timed out after ${timeout_sec}s; forcing pod removal: pod_compose" >&2
  podman pod rm -f pod_compose || true
  podman pod inspect pod_compose >/dev/null 2>&1 || true

  return 0
}
