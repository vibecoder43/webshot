#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: run_devenv_task.sh <log_path> <delegated_devenv_args...>

Runs ./delegated_devenv.sh with stdout/stderr streamed to both console and a log
file, while preserving the command's real exit code (no shell pipelines).

On failure, attempts to print systemd transient unit status (GitHub Actions).
EOF
  exit 2
}

if [[ ${1-} == "--help" || ${1-} == "-h" ]]; then
  usage
fi

log_path=${1-}
shift || true
if [[ -z ${log_path:-} || $# -lt 1 ]]; then
  usage
fi

mkdir -p "$(dirname -- "$log_path")"

repo_root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "${repo_root}"

unit_name=""
if [[ -n ${GITHUB_ACTIONS:-} ]]; then
  # Must match delegated_devenv.sh: we need a stable name to query systemd.
  run_id=${GITHUB_RUN_ID:-0}
  attempt=${GITHUB_RUN_ATTEMPT:-0}
  job=${GITHUB_JOB:-job}
  job=${job//[^A-Za-z0-9_.-]/_}
  unit_name="webshot-gha-devenv-${run_id}-${attempt}-${job}"
fi

set +e
./delegated_devenv.sh "$@" \
  > >(tee -a "$log_path") \
  2> >(tee -a "$log_path" >&2)
cmd_rc=$?
set -e

if [[ ${cmd_rc} -ne 0 && -n ${unit_name} ]]; then
  {
    echo
    echo "==== systemd transient unit status (${unit_name}.service) ===="
    sudo systemctl show \
      -p Id \
      -p Names \
      -p LoadState \
      -p ActiveState \
      -p SubState \
      -p Result \
      -p KillMode \
      -p TimeoutStartUSec \
      -p TimeoutStopUSec \
      -p ExecMainPID \
      -p ExecMainCode \
      -p ExecMainStatus \
      -p ExitCode \
      -p StatusErrno \
      -p TasksCurrent \
      -p ControlGroup \
      -p Delegate \
      -p DelegateControllers \
      "${unit_name}.service" 2>&1 || true
    echo "==== end systemd status ===="
  } | tee -a "$log_path" >&2
fi

exit "${cmd_rc}"
