#!/usr/bin/env bash
set -euo pipefail

status_path=$1
cgroup_stats_path=$2
cgroup_root=$3
cgroup_name=$4
cpu_cores=$5
memory_bytes=$6
seccomp_bpf_path=$7
shift 7
exec 3>"$status_path"
exec 4<"$seccomp_bpf_path"

bwrap_pid=''
browser_cgroup_dir=''

# shellcheck disable=SC2329  # Invoked via trap below.
snapshot_cgroup_stats() {
  [[ -n $browser_cgroup_dir ]] || return 0
  [[ -d $browser_cgroup_dir ]] || return 0
  : >"$cgroup_stats_path" || return 0
  for file in cpu.stat memory.current memory.events io.stat; do
    if [[ -f $browser_cgroup_dir/$file ]]; then
      printf '[%s]\n' "$file" >>"$cgroup_stats_path" 2>/dev/null || true
      cat "$browser_cgroup_dir/$file" >>"$cgroup_stats_path" 2>/dev/null || true
    fi
  done
}

# shellcheck disable=SC2329  # Invoked via trap below.
cleanup() {
  local rc=$?
  if [[ -n $bwrap_pid ]]; then
    kill "$bwrap_pid" 2>/dev/null || true
    wait "$bwrap_pid" 2>/dev/null || true
  fi
  if [[ -n $browser_cgroup_dir ]]; then
    snapshot_cgroup_stats
    printf '%s\n' "$$" >"$cgroup_root/cgroup.procs" 2>/dev/null || true
    for _ in {1..50}; do
      if rmdir "$browser_cgroup_dir" 2>/dev/null; then
        break
      fi
      [[ -d $browser_cgroup_dir ]] || break
      sleep 0.1
    done
  fi
  exit "$rc"
}

trap 'cleanup' EXIT INT TERM

if [[ $cpu_cores != 0 ]]; then
  if [[ $cgroup_root != /* ]]; then
    echo "managed cgroup root must be absolute: $cgroup_root" >&2
    exit 2
  fi
  if [[ ! -f $cgroup_root/cgroup.controllers ]]; then
    echo "managed cgroup root is not available: $cgroup_root" >&2
    exit 2
  fi

  browser_cgroup_dir=$cgroup_root/$cgroup_name
  mkdir -p "$browser_cgroup_dir"

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
  printf '%s\n' "$$" >"$browser_cgroup_dir/cgroup.procs"
fi

"$@" &
bwrap_pid=$!
set +e
wait "$bwrap_pid"
bwrap_rc=$?
set -e
bwrap_pid=''
exit "$bwrap_rc"
