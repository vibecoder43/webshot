#!/usr/bin/env bash
set -euo pipefail

repo_root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

# shellcheck source=shell/lib.sh
source "${repo_root}/shell/lib.sh"

need systemd-run
need devenv

usage() {
  cat >&2 <<'EOF'
usage: delegated_devenv.sh

Starts `devenv --no-tui shell bash` inside a delegated `systemd --user` scope.

Examples:
  delegated_devenv.sh
EOF
  exit 2
}

if [[ ${1-} == "--help" || ${1-} == "-h" ]]; then
  usage
fi

[[ $# -eq 0 ]] || usage

if [[ ${WEBSHOT_DELEGATED_DEVENV:-} == 1 ]]; then
  cd "${repo_root}"
  exec devenv --no-tui shell bash
fi

exec systemd-run \
  --user \
  --scope \
  --quiet \
  --collect \
  --same-dir \
  --setenv=WEBSHOT_DELEGATED_DEVENV=1 \
  --property='Delegate=cpu memory' \
  --description="webshot delegated devenv" \
  bash -lc "cd \"\$1\"; exec devenv --no-tui shell bash" bash "${repo_root}"
