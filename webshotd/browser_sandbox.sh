#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 11 ]]; then
  echo "usage: $0 <proxy-upstream-socket> <cdp-socket> <websocket-path-file> <proxy-listen-port> <devtools-port> <cgroup-root> <cgroup-name> <cpu-cores> <memory-bytes> -- <browser-bin> <browser-args...>" >&2
  exit 2
fi

proxy_upstream_socket=$1
cdp_socket=$2
websocket_path_file=$3
proxy_listen_port=$4
devtools_port=$5
cgroup_root=$6
cgroup_name=$7
cpu_cores=$8
memory_bytes=$9
shift 9

if [[ ${1-} != "--" ]]; then
  echo "missing -- separator before browser command" >&2
  exit 2
fi
shift

if [[ $# -lt 1 ]]; then
  echo "missing browser command" >&2
  exit 2
fi

proxy_pid=''
cdp_pid=''
browser_pid=''
chromium_stderr_path='chromium-stderr.log'
browser_cgroup_dir=''

browserAlive() {
  kill -0 "$browser_pid" 2>/dev/null
}

devtoolsWebsocketPath() {
  [[ -f $chromium_stderr_path ]] || return 0
  sed -n "s#^DevTools listening on ws://127\\.0\\.0\\.1:${devtools_port}\\(/[^[:space:]]*\\)\$#\\1#p" \
    "$chromium_stderr_path" | tail -n 1
}

# shellcheck disable=SC2329  # Invoked via trap below.
cleanup() {
  local rc=$?
  if [[ -n $browser_pid ]]; then
    kill "$browser_pid" 2>/dev/null || true
  fi
  if [[ -n $proxy_pid ]]; then
    kill "$proxy_pid" 2>/dev/null || true
  fi
  if [[ -n $cdp_pid ]]; then
    kill "$cdp_pid" 2>/dev/null || true
  fi
  wait 2>/dev/null || true
  if [[ -n $browser_cgroup_dir ]]; then
    printf '%s\n' "$$" >"$cgroup_root/cgroup.procs" 2>/dev/null || true
    rmdir "$browser_cgroup_dir" 2>/dev/null || true
  fi
  exit "$rc"
}

trap 'cleanup' EXIT INT TERM

rm -f "$chromium_stderr_path"
rm -f "$cdp_socket"
rm -f "$websocket_path_file"

if [[ $cpu_cores != 0 ]]; then
  if [[ $cgroup_root != /* ]]; then
    echo "managed cgroup root must be absolute: $cgroup_root" >&2
    exit 2
  fi
  if [[ ! -f $cgroup_root/cgroup.controllers ]]; then
    echo "managed cgroup root is not available in the sandbox: $cgroup_root" >&2
    exit 2
  fi

  browser_cgroup_dir=$cgroup_root/$cgroup_name
  mkdir -p "$browser_cgroup_dir"
  printf '%s\n' 0 >"$browser_cgroup_dir/cgroup.procs"

  controllers=$(cat "$cgroup_root/cgroup.controllers")
  for controller in cpu memory; do
    if [[ " $controllers " != *" $controller "* ]]; then
      echo "cgroup controller '$controller' is not available" >&2
      exit 2
    fi
  done

  enabled=$(cat "$cgroup_root/cgroup.subtree_control")
  enable_args=()
  for controller in cpu memory; do
    if [[ " $enabled " != *" $controller "* ]]; then
      enable_args+=("+$controller")
    fi
  done
  if (( ${#enable_args[@]} > 0 )); then
    printf '%s\n' "${enable_args[*]}" >"$cgroup_root/cgroup.subtree_control"
  fi

  quota_us=$(( cpu_cores * 100000 ))
  printf '%s %s\n' "$quota_us" "100000" >"$browser_cgroup_dir/cpu.max"
  printf '%s\n' "$memory_bytes" >"$browser_cgroup_dir/memory.max"
fi

socat \
  TCP-LISTEN:"${proxy_listen_port}",bind=127.0.0.1,reuseaddr,fork \
  UNIX-CONNECT:"$proxy_upstream_socket" &
proxy_pid=$!

"$@" 2> >(tee "$chromium_stderr_path" >&2) &
browser_pid=$!

for ((i = 0; i < 300; i++)); do
  if ! browserAlive; then
    echo "chromium exited before advertising devtools on 127.0.0.1:${devtools_port}" >&2
    exit 1
  fi

  websocket_path=$(devtoolsWebsocketPath)
  if [[ -n $websocket_path ]]; then
    socat \
      UNIX-LISTEN:"$cdp_socket",unlink-early \
      TCP-CONNECT:"127.0.0.1:${devtools_port}" &
    cdp_pid=$!
    printf '%s' "$websocket_path" >"$websocket_path_file"
    set +e
    wait "$browser_pid"
    browser_rc=$?
    set -e
    exit "$browser_rc"
  fi

  sleep 0.1
done

echo "timed out waiting for chromium to advertise devtools on 127.0.0.1:${devtools_port}" >&2
exit 1
