#!/usr/bin/env bash
set -euo pipefail

filter_relevant_config() {
  # Prefer ripgrep if present, but fall back to grep.
  local pattern='^(substituters|trusted-public-keys|trusted-substituters|require-sigs|builders|builders-use-substitutes|fallback|max-jobs|cores|sandbox|sandbox-fallback|system-features)\b'
  if command -v rg >/dev/null 2>&1; then
    rg -n "${pattern}" || true
  else
    grep -En "${pattern}" || true
  fi
}

echo "== basic =="
id || true
uname -a || true
echo "PWD=$PWD"
echo "HOME=${HOME:-<unset>}"
echo

echo "== nix version =="
command -v nix || true
nix --version || true
echo

echo "== /etc/nix =="
ls -la /etc/nix 2>/dev/null || true
echo "--- /etc/nix/nix.conf ---"
cat /etc/nix/nix.conf 2>/dev/null || true
echo "--- /etc/nix/machines ---"
cat /etc/nix/machines 2>/dev/null || true
echo

echo "== nix show-config (relevant) =="
nix show-config 2>/dev/null | filter_relevant_config
echo

echo "== writeability checks =="
for p in "${HOME:-}" "${HOME:-}/.cache" "${HOME:-}/.cache/nix" /nix /nix/store /build; do
  [ -n "$p" ] || continue
  echo "-- $p"
  ls -ld "$p" 2>/dev/null || true
  ( test -w "$p" && echo "writable: yes" ) || echo "writable: no"
done
echo

echo "== network sanity (no secrets) =="
getent hosts cache.nixos.org 2>/dev/null || true
if command -v curl >/dev/null 2>&1; then
  curl -I --max-time 10 https://cache.nixos.org/ 2>/dev/null | head -n 5 || true
elif command -v wget >/dev/null 2>&1; then
  wget -S --spider -T 10 https://cache.nixos.org/ 2>&1 | head -n 20 || true
else
  echo "no curl/wget available"
fi
echo

echo "== remote builders sanity (if configured) =="
if [ -f /etc/nix/machines ]; then
  echo "--- /etc/nix/machines entries ---"
  sed -n '1,200p' /etc/nix/machines || true
fi
echo

echo "== can we substitute skopeo (force no remote builders) =="
NIXPKGS_REV="${NIXPKGS_REV:-}"
if [ -z "${NIXPKGS_REV}" ]; then
  if [ -f devenv.lock ] && command -v nix >/dev/null 2>&1; then
    NIXPKGS_REV="$(nix --extra-experimental-features nix-command eval --raw --impure --expr \
      'let lock = builtins.fromJSON (builtins.readFile ./devenv.lock); in lock.nodes.nixpkgs.locked.rev' 2>/dev/null || true)"
  fi
fi
if [ -z "${NIXPKGS_REV}" ] && [ -f flake.lock ] && command -v nix >/dev/null 2>&1; then
  NIXPKGS_REV="$(nix --extra-experimental-features nix-command eval --raw --impure --expr \
    'let lock = builtins.fromJSON (builtins.readFile ./flake.lock); in lock.nodes.nixpkgs.locked.rev' 2>/dev/null || true)"
fi
if [ -n "${NIXPKGS_REV}" ]; then
  echo "Using pinned nixpkgs rev: ${NIXPKGS_REV}"
  NIXPKGS_FLAKE="github:NixOS/nixpkgs/${NIXPKGS_REV}"
else
  echo "No pinned nixpkgs rev found; falling back to flake-registry nixpkgs"
  NIXPKGS_FLAKE="nixpkgs"
fi
echo

set +e
nix --extra-experimental-features 'nix-command flakes' \
  build -L --no-link --show-trace \
  --option builders "" \
  "${NIXPKGS_FLAKE}#skopeo"
rc=$?
set -e
echo "exit=$rc"
echo

echo "== can we build anything locally (force no remote builders) =="
set +e
nix --extra-experimental-features 'nix-command flakes' \
  build -L --no-link --show-trace \
  --option builders "" \
  "${NIXPKGS_FLAKE}#hello"
rc=$?
set -e
echo "exit=$rc"
echo

echo "== done =="
