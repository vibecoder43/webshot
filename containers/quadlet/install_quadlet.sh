#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd -- "${script_dir}/../.." && pwd)"

containers_dest="${XDG_CONFIG_HOME:-"${HOME}/.config"}/containers/systemd"
user_units_dest="${XDG_CONFIG_HOME:-"${HOME}/.config"}/systemd/user"
runtime_dest=""
if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
  runtime_dest="${XDG_RUNTIME_DIR}/containers/systemd"
fi

mkdir -p "${containers_dest}" "${user_units_dest}"
if [[ -n "${runtime_dest}" ]]; then
  mkdir -p "${runtime_dest}"
fi

echo "Installing Webshot Quadlet units (symlinks) into: ${containers_dest}"

systemctl --user import-environment PATH

echo "Setting user systemd environment: WEBSHOT_ROOT=${root}"
if [[ -n "${WEBSHOT_RUNTIME_LD_LIBRARY_PATH:-}" ]]; then
  systemctl --user set-environment \
    "WEBSHOT_ROOT=${root}" \
    "WEBSHOT_BUILD_DIR=/tmp/build-webshot-san" \
    "WEBSHOT_RUNTIME_LD_LIBRARY_PATH=${WEBSHOT_RUNTIME_LD_LIBRARY_PATH}"
else
  systemctl --user set-environment \
    "WEBSHOT_ROOT=${root}" \
    "WEBSHOT_BUILD_DIR=/tmp/build-webshot-san"
  echo "Note: WEBSHOT_RUNTIME_LD_LIBRARY_PATH is not set; host webshot.service may fail to start." >&2
  echo "Tip: run this script under devenv shell/direnv so it can export the runtime library path." >&2
fi

link_unit() {
  local src_rel="$1"
  local dst="$2"
  if [[ ! -f "${root}/${src_rel}" ]]; then
    echo "Skipping missing unit template: ${src_rel}" >&2
    return 0
  fi
  ln -sf "${root}/${src_rel}" "${dst}"
  echo "Linked ${src_rel} -> ${dst}"
}

link_quadlet_unit() {
  local src_rel="$1"
  local name
  name="$(basename "${src_rel}")"
  link_unit "${src_rel}" "${containers_dest}/${name}"
  if [[ -n "${runtime_dest}" ]]; then
    link_unit "${src_rel}" "${runtime_dest}/${name}"
  fi
}

link_user_unit() {
  local src_rel="$1"
  local name
  name="$(basename "${src_rel}")"
  link_unit "${src_rel}" "${user_units_dest}/${name}"
}

quadlet_units=(
  containers/quadlet/webshot-crawler.network
  containers/quadlet/webshot-postgres.container
  containers/quadlet/webshot-seaweed.container
  containers/quadlet/webshot-scalar.container
  containers/quadlet/webshot-test-target.container
  containers/quadlet/webshot-reverse-proxy.container
)

for unit in "${quadlet_units[@]}"; do
  link_quadlet_unit "${unit}"
done

user_units=(
  containers/quadlet/webshot-stack.target
  containers/quadlet/webshot-prod-stack.target
  containers/quadlet/webshot-debug-stack.target
  containers/quadlet/webshot-prod-debug-stack.target
  containers/quadlet/webshot.service
  containers/quadlet/webshot-prod.service
)

for unit in "${user_units[@]}"; do
  link_user_unit "${unit}"
done

echo "Reloading user systemd units"
systemctl --user daemon-reload
