#!/usr/bin/env bash
set -Eeuo pipefail

die() { echo "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1" >&2; exit 2; }; }
need nix

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "${root}"

nix_flake_args=(--extra-experimental-features 'nix-command flakes')
devenv_pin_expr=$'let\n  lock = builtins.fromJSON (builtins.readFile ./devenv.lock);\n  locked = lock.nodes.devenv.locked or (throw "devenv.lock is missing nodes.devenv.locked");\n  get = name: if builtins.hasAttr name locked then builtins.getAttr name locked else "";\nin builtins.concatStringsSep "\\n" [ (get "owner") (get "repo") (get "rev") (get "narHash") ]'
mapfile -t devenv_pin < <(nix "${nix_flake_args[@]}" eval --impure --raw --expr "${devenv_pin_expr}")

[[ ${#devenv_pin[@]} -eq 4 ]] || die "Failed to read devenv bootstrap pin from devenv.lock"

devenv_owner="${devenv_pin[0]}"
devenv_repo="${devenv_pin[1]}"
devenv_ref="${devenv_pin[2]}"
devenv_nar_hash="${devenv_pin[3]}"

[[ -n "${devenv_owner}" ]] || die "devenv.lock is missing nodes.devenv.locked.owner"
[[ -n "${devenv_repo}" ]] || die "devenv.lock is missing nodes.devenv.locked.repo"
[[ -n "${devenv_ref}" ]] || die "devenv.lock is missing nodes.devenv.locked.rev"
[[ -n "${devenv_nar_hash}" ]] || die "devenv.lock is missing nodes.devenv.locked.narHash"
[[ "${devenv_owner}/${devenv_repo}" == "cachix/devenv" ]] || die "devenv.lock bootstrap target must stay on cachix/devenv"

# The lock node is the module subtree, but the CLI lives at the same revision's
# root flake. Build the installable explicitly without the node's dir field.
devenv_installable="github:${devenv_owner}/${devenv_repo}/${devenv_ref}?narHash=${devenv_nar_hash}#devenv"

devenv_args=(--no-tui --no-eval-cache --refresh-task-cache)

# CI logs often hide the underlying Nix eval error; opt into verbose tracing when requested.
if [[ "${DEVENV_CI_DEBUG:-}" == "1" ]]; then
  devenv_args+=(--log-format tracing-pretty)
  devenv_args+=(--verbose)
  devenv_args+=(--nix-option show-trace true)
fi

exec nix "${nix_flake_args[@]}" run "${devenv_installable}" -- "${devenv_args[@]}" "$@"
