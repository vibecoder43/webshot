#!/usr/bin/env bash
set -Eeuo pipefail

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 2; }; }

need podman

mode="${1:-}"
verbose="false"
if [[ "${2:-}" == "--verbose" ]]; then
  verbose="true"
elif [[ -n "${2:-}" ]]; then
  echo "Usage: infra_ready.sh <dev|prodlike> [--verbose]" >&2
  exit 2
fi

case "${mode}" in
  dev) names=(egress-proxy servicedb seaweedfs webshot_scalar webshot_reverse_proxy webshot-test-target) ;;
  prodlike) names=(egress-proxy servicedb) ;;
  *)
    echo "Usage: infra_ready.sh <dev|prodlike> [--verbose]" >&2
    exit 2
    ;;
esac

for name in "${names[@]}"; do
  run_status="$(podman inspect -f '{{.State.Status}}' "${name}" 2>/dev/null || true)"
  if [[ "${run_status}" != "running" ]]; then
    if [[ "${verbose}" == "true" ]]; then
      if [[ -z "${run_status}" ]]; then
        echo "infra: ${name}: missing" >&2
      else
        echo "infra: ${name}: not running (status='${run_status}')" >&2
      fi
      podman ps -a --filter "name=^${name}$" --format 'infra: {{.Names}}: {{.Status}}' 2>/dev/null || true
      podman logs --tail=50 "${name}" 2>/dev/null || true
    fi
    exit 1
  fi

  health_status="$(podman inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{end}}' "${name}" 2>/dev/null || true)"
  if [[ -n "${health_status}" && "${health_status}" != "healthy" ]]; then
    if [[ "${verbose}" == "true" ]]; then
      echo "infra: ${name}: health='${health_status}'" >&2
      podman logs --tail=50 "${name}" 2>/dev/null || true
    fi
    exit 1
  fi
done

exit 0

