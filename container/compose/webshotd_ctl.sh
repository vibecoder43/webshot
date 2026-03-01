#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "${script_dir}/../.." && pwd)"
# shellcheck source=shell/lib.sh
. "${root}/shell/lib.sh"

on_err() {
  local exit_code=$?
  echo "Error: command failed (exit=${exit_code}) at ${BASH_SOURCE[0]}:${BASH_LINENO[0]}: ${BASH_COMMAND}" >&2
  exit "${exit_code}"
}
trap on_err ERR

usage() {
  cat >&2 <<'EOF'
Usage: webshotd_ctl.sh <dev|prodlike> <up|down|status|logs>

Uses:
  - WEBSHOTD_BUILD_DIR (required for up)
  - WEBSHOTD_RUNTIME_LD_LIBRARY_PATH (required for up)
  - CPU_LIMIT (optional override; e.g. "4c" or "0.5c")
  - DEPLOY_VCPU_LIMIT (optional override; millicores, e.g. "4000")
  - WEBSHOTD_STATE_DIR (optional; defaults to ${XDG_RUNTIME_DIR:-/tmp}/webshotd-${UID})

Timeout env vars (optional):
  - INFRA_READY_TIMEOUT_SEC (default: 240)
  - WEBSHOTD_READY_TIMEOUT_SEC (default: 60)
  - WEBSHOTD_STOP_TIMEOUT_SEC (default: 30)
EOF
  exit 2
}

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

cd -- "${root}"

runtime_root="${XDG_RUNTIME_DIR:-/tmp}"
state_root="${WEBSHOTD_STATE_DIR:-${runtime_root}/webshotd-${UID}}"
state_dir="${state_root}/${mode}"
mkdir -p "${state_dir}"

webshotd_name="webshotd"
webshotd_pid_file="${state_dir}/${webshotd_name}.pid"
webshotd_log_file="${state_dir}/${webshotd_name}.log"

infra_ready_timeout_sec="${INFRA_READY_TIMEOUT_SEC:-240}"
webshotd_ready_timeout_sec="${WEBSHOTD_READY_TIMEOUT_SEC:-60}"
webshotd_stop_timeout_sec="${WEBSHOTD_STOP_TIMEOUT_SEC:-30}"

infra_up() {
  case "${mode}" in
    dev)
      bash container/compose/infra_dev_up.sh
      ;;
    prodlike)
      bash container/compose/infra_prodlike_up.sh
      ;;
  esac

  local deadline=$((SECONDS + infra_ready_timeout_sec))
  while true; do
    if bash container/compose/infra_ready.sh "${mode}" >/dev/null 2>&1; then
      return 0
    fi
    if ((SECONDS >= deadline)); then
      echo "Infra did not become ready within ${infra_ready_timeout_sec}s." >&2
      bash container/compose/infra_ready.sh "${mode}" --verbose || true
      return 1
    fi
    sleep 1
  done
}

infra_down() {
  case "${mode}" in
    dev)
      bash container/compose/infra_dev_down.sh
      ;;
    prodlike)
      bash container/compose/infra_prodlike_down.sh
      ;;
  esac
}

pid_is_running() {
  local pid="$1"
  [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1
}

webshotd_cmd() {
  case "${mode}" in
    dev)
      echo "${WEBSHOTD_BUILD_DIR}/webshotd --config config/static_config.yaml --config_vars config/config_vars.debug.yaml"
      ;;
    prodlike)
      echo "${WEBSHOTD_BUILD_DIR}/webshotd --config config/static_config.yaml --config_vars config/config_vars.prod.yaml --config_vars_override config/config_vars.prod.debug.yaml"
      ;;
  esac
}

min_int() {
  local a="$1"
  local b="$2"
  if ((a < b)); then
    echo "${a}"
  else
    echo "${b}"
  fi
}

cpuset_cpu_count() {
  local cpuset_raw="$1"
  local cpuset="${cpuset_raw//[[:space:]]/}"
  if [[ -z "${cpuset}" ]]; then
    return 1
  fi

  local count=0
  local part
  local start
  local end
  for part in ${cpuset//,/ }; do
    if [[ "${part}" =~ ^[0-9]+$ ]]; then
      ((count++))
      continue
    fi
    if [[ "${part}" =~ ^([0-9]+)-([0-9]+)$ ]]; then
      start="${BASH_REMATCH[1]}"
      end="${BASH_REMATCH[2]}"
      if ((end < start)); then
        return 1
      fi
      count=$((count + (end - start + 1)))
      continue
    fi
    return 1
  done

  if ((count <= 0)); then
    return 1
  fi
  echo "${count}"
}

read_file_trimmed() {
  local path="$1"
  [[ -f "${path}" ]] || return 1
  local data
  data="$(<"${path}")"
  data="${data#"${data%%[![:space:]]*}"}"
  data="${data%"${data##*[![:space:]]}"}"
  [[ -n "${data}" ]] || return 1
  echo "${data}"
}

infer_cpu_threads() {
  local threads=""
  local hw_threads=""

  if command -v nproc >/dev/null 2>&1; then
    hw_threads="$(nproc 2>/dev/null || true)"
  elif command -v getconf >/dev/null 2>&1; then
    hw_threads="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
  fi

  # cgroup v2
  local cgroup_v2_path=""
  cgroup_v2_path="$(awk -F: '$1 == "0" && $2 == "" {print $3}' /proc/self/cgroup 2>/dev/null | head -n 1 || true)"
  if [[ -n "${cgroup_v2_path}" && -d "/sys/fs/cgroup${cgroup_v2_path}" ]]; then
    local cpu_max
    cpu_max="$(read_file_trimmed "/sys/fs/cgroup${cgroup_v2_path}/cpu.max" || true)"
    if [[ -n "${cpu_max}" ]]; then
      local quota
      local period
      quota="$(cut -d' ' -f1 <<<"${cpu_max}" 2>/dev/null || true)"
      period="$(cut -d' ' -f2 <<<"${cpu_max}" 2>/dev/null || true)"
      if [[ "${quota}" =~ ^[0-9]+$ && "${period}" =~ ^[0-9]+$ ]] && ((period > 0)) && ((quota > 0)); then
        threads="$(((quota + period - 1) / period))"
      fi
    fi

    local cpuset_effective
    cpuset_effective="$(read_file_trimmed "/sys/fs/cgroup${cgroup_v2_path}/cpuset.cpus.effective" || true)"
    if [[ -n "${cpuset_effective}" ]]; then
      local cpuset_threads
      cpuset_threads="$(cpuset_cpu_count "${cpuset_effective}" || true)"
      if [[ "${cpuset_threads}" =~ ^[0-9]+$ ]] && ((cpuset_threads > 0)); then
        if [[ -n "${threads}" ]]; then
          threads="$(min_int "${threads}" "${cpuset_threads}")"
        else
          threads="${cpuset_threads}"
        fi
      fi
    fi
  fi

  # cgroup v1 (best-effort; may not exist on modern hosts)
  if [[ -z "${threads}" ]]; then
    local cpu_cg_path=""
    cpu_cg_path="$(awk -F: '($2 ~ /(^|,)cpu(,|$)/) {print $3}' /proc/self/cgroup 2>/dev/null | head -n 1 || true)"
    if [[ -n "${cpu_cg_path}" ]]; then
      local cpu_mount=""
      if [[ -d "/sys/fs/cgroup/cpu${cpu_cg_path}" ]]; then
        cpu_mount="/sys/fs/cgroup/cpu${cpu_cg_path}"
      elif [[ -d "/sys/fs/cgroup/cpu,cpuacct${cpu_cg_path}" ]]; then
        cpu_mount="/sys/fs/cgroup/cpu,cpuacct${cpu_cg_path}"
      fi
      if [[ -n "${cpu_mount}" ]]; then
        local quota_us
        local period_us
        quota_us="$(read_file_trimmed "${cpu_mount}/cpu.cfs_quota_us" || true)"
        period_us="$(read_file_trimmed "${cpu_mount}/cpu.cfs_period_us" || true)"
        if [[ "${quota_us}" =~ ^-?[0-9]+$ && "${period_us}" =~ ^[0-9]+$ ]] && ((period_us > 0)) && ((quota_us > 0)); then
          threads="$(((quota_us + period_us - 1) / period_us))"
        fi
      fi
    fi
  fi

  if [[ -z "${threads}" && "${hw_threads}" =~ ^[0-9]+$ ]] && ((hw_threads > 0)); then
    threads="${hw_threads}"
  fi

  if [[ -n "${threads}" && "${hw_threads}" =~ ^[0-9]+$ ]] && ((hw_threads > 0)) && ((threads > hw_threads)); then
    threads="${hw_threads}"
  fi

  if [[ "${threads}" =~ ^[0-9]+$ ]] && ((threads > 0)); then
    echo "${threads}"
    return 0
  fi

  return 1
}

webshotd_ensure_cpu_limit() {
  if [[ -n "${CPU_LIMIT:-}" ]]; then
    if [[ ! "${CPU_LIMIT}" =~ ^[0-9]+([.][0-9]+)?c$ ]]; then
      die "CPU_LIMIT must look like '4c' or '0.5c', got: '${CPU_LIMIT}'"
    fi
    if [[ "${CPU_LIMIT}" =~ ^0+([.][0]+)?c$ ]]; then
      die "CPU_LIMIT must be > 0, got: '${CPU_LIMIT}'"
    fi
    return 0
  fi

  if [[ -n "${DEPLOY_VCPU_LIMIT:-}" ]]; then
    if [[ ! "${DEPLOY_VCPU_LIMIT}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
      die "DEPLOY_VCPU_LIMIT must be numeric millicores (e.g. '4000'), got: '${DEPLOY_VCPU_LIMIT}'"
    fi
    if [[ "${DEPLOY_VCPU_LIMIT}" =~ ^0+([.][0]+)?$ ]]; then
      die "DEPLOY_VCPU_LIMIT must be > 0, got: '${DEPLOY_VCPU_LIMIT}'"
    fi
    return 0
  fi

  local threads
  if ! threads="$(infer_cpu_threads)"; then
    die "Failed to infer CPU thread count. Set CPU_LIMIT (e.g. '4c') or DEPLOY_VCPU_LIMIT (e.g. '4000')."
  fi
  export CPU_LIMIT="${threads}c"
}

webshotd_wait_ready() {
  # userver monitor handler listens on listener-monitor (see config/static_config.yaml)
  local urls=(
    "http://127.0.0.1:8081/service/monitor?format=json"
    "http://127.0.0.1:8081/service/monitor?format=tskv"
    "http://127.0.0.1:8081/service/monitor"
  )
  local deadline=$((SECONDS + webshotd_ready_timeout_sec))

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
        if python3 - <<PY >/dev/null 2>&1
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
        then
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
      echo "${webshotd_name} did not become ready within ${webshotd_ready_timeout_sec}s (monitor listener on 127.0.0.1:8081)." >&2
      if [[ -f "${webshotd_log_file}" ]]; then
        echo "--- ${webshotd_name} log (tail)" >&2
        tail -n 200 "${webshotd_log_file}" >&2 || true
      fi
      return 1
    fi
    sleep 1
  done
}

webshotd_start() {
  need_env WEBSHOTD_BUILD_DIR
  need_env WEBSHOTD_RUNTIME_LD_LIBRARY_PATH
  webshotd_ensure_cpu_limit

  if [[ -f "${webshotd_pid_file}" ]]; then
    local pid
    pid="$(cat "${webshotd_pid_file}" 2>/dev/null || true)"
    if pid_is_running "${pid}"; then
      return 0
    fi
    rm -f -- "${webshotd_pid_file}"
  fi

  local cmd
  cmd="$(webshotd_cmd)"
  : >>"${webshotd_log_file}"

  echo "Starting ${webshotd_name} (${mode})..." >&2
  (
    export LD_LIBRARY_PATH="${WEBSHOTD_RUNTIME_LD_LIBRARY_PATH}"
    cd -- "${root}"
    nohup bash -c "${cmd}" >>"${webshotd_log_file}" 2>&1 &
    echo "$!" >"${webshotd_pid_file}"
  )
}

webshotd_stop() {
  if [[ ! -f "${webshotd_pid_file}" ]]; then
    return 0
  fi

  local pid
  pid="$(cat "${webshotd_pid_file}" 2>/dev/null || true)"
  if ! pid_is_running "${pid}"; then
    rm -f -- "${webshotd_pid_file}"
    return 0
  fi

  echo "Stopping ${webshotd_name} (pid=${pid})..." >&2
  kill "${pid}" >/dev/null 2>&1 || true

  local deadline=$((SECONDS + webshotd_stop_timeout_sec))
  while pid_is_running "${pid}"; do
    if ((SECONDS >= deadline)); then
      echo "${webshotd_name} did not stop within ${webshotd_stop_timeout_sec}s, killing pid=${pid}." >&2
      kill -KILL "${pid}" >/dev/null 2>&1 || true
      break
    fi
    sleep 1
  done

  rm -f -- "${webshotd_pid_file}"
}

print_status() {
  echo "Mode: ${mode}"

  if bash container/compose/infra_ready.sh "${mode}" >/dev/null 2>&1; then
    echo "Infra: ready"
  else
    echo "Infra: not ready"
    bash container/compose/infra_ready.sh "${mode}" --verbose || true
  fi

  if [[ -f "${webshotd_pid_file}" ]]; then
    local pid
    pid="$(cat "${webshotd_pid_file}" 2>/dev/null || true)"
    if pid_is_running "${pid}"; then
      echo "${webshotd_name}: running (pid=${pid})"
    else
      echo "${webshotd_name}: stale pid file (pid=${pid})"
    fi
  else
    echo "${webshotd_name}: not running (no pid file)"
  fi
}

case "${action}" in
  up)
    infra_up
    webshotd_start
    webshotd_wait_ready
    ;;
  down)
    webshotd_stop
    infra_down
    ;;
  status)
    print_status
    ;;
  logs)
    : >>"${webshotd_log_file}"
    tail -n 200 -f "${webshotd_log_file}"
    ;;
esac
