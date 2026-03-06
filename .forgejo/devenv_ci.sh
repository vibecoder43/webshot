#!/usr/bin/env bash
set -Eeuo pipefail

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 2; }; }
need nix

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "${root}"

# Pinned to devenv CLI v1.11.1 (commit behind tag v1.11.1).
# Override with DEVENV_REF if you need to bump it deliberately.
devenv_ref="${DEVENV_REF:-51440964cd26a47e90064f9d59aa230a5cefc88b}"

devenv_args=()

# CI logs often hide the underlying Nix eval error; opt into verbose tracing when requested.
if [[ "${DEVENV_CI_DEBUG:-}" == "1" ]]; then
  devenv_args+=(--log-format tracing-pretty)
  devenv_args+=(--verbose)
  devenv_args+=(--nix-option show-trace true)
fi

exec nix --extra-experimental-features 'nix-command flakes' \
  run "github:cachix/devenv/${devenv_ref}#devenv" -- "${devenv_args[@]}" "$@"
