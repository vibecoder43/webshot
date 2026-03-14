#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 ]]; then
  echo "usage: $0 <proxy-upstream-socket> <cdp-socket> -- <browser-bin> <browser-args...>" >&2
  exit 2
fi

proxy_upstream_socket=$1
cdp_socket=$2
shift 2

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
  exit "$rc"
}

trap cleanup EXIT INT TERM

rm -f "$cdp_socket"

socat \
  TCP-LISTEN:3128,bind=127.0.0.1,reuseaddr,fork \
  UNIX-CONNECT:"$proxy_upstream_socket" &
proxy_pid=$!

socat \
  UNIX-LISTEN:"$cdp_socket",fork,unlink-early \
  TCP-CONNECT:127.0.0.1:9222 &
cdp_pid=$!

"$@" &
browser_pid=$!
wait "$browser_pid"
