#!/usr/bin/env bash
set -Eeuo pipefail

on_err() {
  local exit_code=$?
  echo "Error: command failed (exit=${exit_code}) at ${BASH_SOURCE[0]}:${BASH_LINENO[0]}: ${BASH_COMMAND}" >&2
  exit "${exit_code}"
}
trap on_err ERR

usage() {
  cat >&2 <<'EOF'
Usage: webshot_ctl.sh <dev|prodlike> <up|down|status|logs>

Uses:
  - WEBSHOT_BUILD_DIR (required for up)
  - WEBSHOT_RUNTIME_LD_LIBRARY_PATH (required for up)
  - WEBSHOT_STATE_DIR (optional; defaults to <repo>/.cache/webshot)

Timeout env vars (optional):
  - WEBSHOT_INFRA_READY_TIMEOUT_SEC (default: 240)
  - WEBSHOT_WEBSHOT_READY_TIMEOUT_SEC (default: 60)
  - WEBSHOT_WEBSHOT_STOP_TIMEOUT_SEC (default: 30)
EOF
  exit 2
}

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 2; }; }
need_env() { [[ -n "${!1:-}" ]] || { echo "Missing required env var: ${1}" >&2; exit 2; }; }

mode="${1:-}"
action="${2:-}"

case "${mode}" in
  dev|prodlike) ;;
  *) usage ;;
esac
case "${action}" in
  up|down|status|logs) ;;
  *) usage ;;
esac

need bash
need podman
need timeout

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "${script_dir}/../.." && pwd)"
cd -- "${root}"

state_root="${WEBSHOT_STATE_DIR:-${root}/.cache/webshot}"
state_dir="${state_root}/${mode}"
mkdir -p "${state_dir}"

webshot_pid_file="${state_dir}/webshot.pid"
webshot_log_file="${state_dir}/webshot.log"

infra_ready_timeout_sec="${WEBSHOT_INFRA_READY_TIMEOUT_SEC:-240}"
webshot_ready_timeout_sec="${WEBSHOT_WEBSHOT_READY_TIMEOUT_SEC:-60}"
webshot_stop_timeout_sec="${WEBSHOT_WEBSHOT_STOP_TIMEOUT_SEC:-30}"

infra_up() {
  case "${mode}" in
    dev) bash containers/compose/infra_dev_up.sh ;;
    prodlike) bash containers/compose/infra_prodlike_up.sh ;;
  esac

  local deadline=$((SECONDS + infra_ready_timeout_sec))
  while true; do
    if bash containers/compose/infra_ready.sh "${mode}" >/dev/null 2>&1; then
      return 0
    fi
    if ((SECONDS >= deadline)); then
      echo "Infra did not become ready within ${infra_ready_timeout_sec}s." >&2
      bash containers/compose/infra_ready.sh "${mode}" --verbose || true
      return 1
    fi
    sleep 1
  done
}

infra_down() {
  case "${mode}" in
    dev) bash containers/compose/infra_dev_down.sh ;;
    prodlike) bash containers/compose/infra_prodlike_down.sh ;;
  esac
}

pid_is_running() {
  local pid="$1"
  [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1
}

webshot_cmd() {
  case "${mode}" in
    dev)
      echo "${WEBSHOT_BUILD_DIR}/webshot --config config/static_config.yaml --config_vars config/config_vars.debug.yaml"
      ;;
    prodlike)
      echo "${WEBSHOT_BUILD_DIR}/webshot --config config/static_config.yaml --config_vars config/config_vars.prod.yaml --config_vars_override config/config_vars.prod.debug.yaml"
      ;;
  esac
}

webshot_wait_ready() {
  # userver monitor handler listens on listener-monitor (see config/static_config.yaml)
  local urls=(
    "http://127.0.0.1:8081/service/monitor?format=json"
    "http://127.0.0.1:8081/service/monitor?format=tskv"
    "http://127.0.0.1:8081/service/monitor"
  )
  local deadline=$((SECONDS + webshot_ready_timeout_sec))

  http_ok() {
    local url
    for url in "${urls[@]}"; do
      if command -v curl >/dev/null 2>&1; then
        local code
        code="$(curl -sS --max-time 1 -o /dev/null -w '%{http_code}' "${url}" 2>/dev/null || true)"
        if [[ "${code}" != "000" && "${code}" -lt 500 ]]; then
          return 0
        fi
        continue
      fi
      if command -v python3 >/dev/null 2>&1; then
        python3 - <<PY >/dev/null 2>&1
import urllib.request
import urllib.error
import sys

url = "${url}"
try:
  with urllib.request.urlopen(url, timeout=1) as r:
    sys.exit(0 if r.status < 500 else 1)
except urllib.error.HTTPError as e:
  sys.exit(0 if e.code < 500 else 1)
except Exception:
  sys.exit(1)
PY
        if [[ "$?" -eq 0 ]]; then
          return 0
        fi
        continue
      fi
      break
    done
    return 2
  }

  while true; do
    if http_ok; then
      return 0
    fi
    if ((SECONDS >= deadline)); then
      echo "webshot did not become ready within ${webshot_ready_timeout_sec}s (monitor listener on 127.0.0.1:8081)." >&2
      if [[ -f "${webshot_log_file}" ]]; then
        echo "--- webshot log (tail)" >&2
        tail -n 200 "${webshot_log_file}" >&2 || true
      fi
      return 1
    fi
    sleep 1
  done
}

webshot_start() {
  need_env WEBSHOT_BUILD_DIR
  need_env WEBSHOT_RUNTIME_LD_LIBRARY_PATH

  if [[ -f "${webshot_pid_file}" ]]; then
    local pid
    pid="$(cat "${webshot_pid_file}" 2>/dev/null || true)"
    if pid_is_running "${pid}"; then
      return 0
    fi
    rm -f -- "${webshot_pid_file}"
  fi

  local cmd
  cmd="$(webshot_cmd)"
  : >>"${webshot_log_file}"

  echo "Starting webshot (${mode})..." >&2
  (
    export LD_LIBRARY_PATH="${WEBSHOT_RUNTIME_LD_LIBRARY_PATH}"
    if [[ "${mode}" == "dev" ]]; then
      export CPU_LIMIT="4c"
    fi
    cd -- "${root}"
    nohup bash -c "${cmd}" >>"${webshot_log_file}" 2>&1 &
    echo "$!" >"${webshot_pid_file}"
  )
}

webshot_stop() {
  if [[ ! -f "${webshot_pid_file}" ]]; then
    return 0
  fi

  local pid
  pid="$(cat "${webshot_pid_file}" 2>/dev/null || true)"
  if ! pid_is_running "${pid}"; then
    rm -f -- "${webshot_pid_file}"
    return 0
  fi

  echo "Stopping webshot (pid=${pid})..." >&2
  kill "${pid}" >/dev/null 2>&1 || true

  local deadline=$((SECONDS + webshot_stop_timeout_sec))
  while pid_is_running "${pid}"; do
    if ((SECONDS >= deadline)); then
      echo "webshot did not stop within ${webshot_stop_timeout_sec}s, killing pid=${pid}." >&2
      kill -KILL "${pid}" >/dev/null 2>&1 || true
      break
    fi
    sleep 1
  done

  rm -f -- "${webshot_pid_file}"
}

print_status() {
  echo "Mode: ${mode}"

  if bash containers/compose/infra_ready.sh "${mode}" >/dev/null 2>&1; then
    echo "Infra: ready"
  else
    echo "Infra: not ready"
    bash containers/compose/infra_ready.sh "${mode}" --verbose || true
  fi

  if [[ -f "${webshot_pid_file}" ]]; then
    local pid
    pid="$(cat "${webshot_pid_file}" 2>/dev/null || true)"
    if pid_is_running "${pid}"; then
      echo "webshot: running (pid=${pid})"
    else
      echo "webshot: stale pid file (pid=${pid})"
    fi
  else
    echo "webshot: not running (no pid file)"
  fi
}

case "${action}" in
  up)
    infra_up
    webshot_start
    webshot_wait_ready
    ;;
  down)
    webshot_stop
    infra_down
    ;;
  status)
    print_status
    ;;
  logs)
    : >>"${webshot_log_file}"
    tail -n 200 -f "${webshot_log_file}"
    ;;
esac
