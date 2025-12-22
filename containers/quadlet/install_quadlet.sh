#!/usr/bin/env bash
set -euo pipefail

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

root="${PWD}"
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

for unit in containers/quadlet/webshot-crawler.network \
            containers/quadlet/webshot-postgres.container \
            containers/quadlet/webshot-seaweed.container \
            containers/quadlet/webshot-scalar.container \
            containers/quadlet/webshot-test-target.container \
            containers/quadlet/webshot-reverse-proxy.container; do
  if [[ ! -f "${unit}" ]]; then
    echo "Skipping missing unit template: ${unit}" >&2
    continue
  fi
  name="$(basename "${unit}")"
  # Symlink from Quadlet search paths back into the repo.
  target="${containers_dest}/${name}"
  ln -sf "${root}/${unit}" "${target}"
  echo "Linked ${unit} -> ${target}"

  if [[ -n "${runtime_dest}" ]]; then
    rt_target="${runtime_dest}/${name}"
    ln -sf "${root}/${unit}" "${rt_target}"
    echo "Linked ${unit} -> ${rt_target}"
  fi
done

stack_unit_src="containers/quadlet/webshot-stack.target"
if [[ -f "${stack_unit_src}" ]]; then
  stack_target="${user_units_dest}/webshot-stack.target"
  ln -sf "${root}/${stack_unit_src}" "${stack_target}"
  echo "Linked ${stack_unit_src} -> ${stack_target}"
fi

debug_stack_src="containers/quadlet/webshot-debug-stack.target"
if [[ -f "${debug_stack_src}" ]]; then
  debug_stack_target="${user_units_dest}/webshot-debug-stack.target"
  ln -sf "${root}/${debug_stack_src}" "${debug_stack_target}"
  echo "Linked ${debug_stack_src} -> ${debug_stack_target}"
fi

host_service_src="containers/quadlet/webshot.service"
if [[ -f "${host_service_src}" ]]; then
  host_service_target="${user_units_dest}/webshot.service"
  ln -sf "${root}/${host_service_src}" "${host_service_target}"
  echo "Linked ${host_service_src} -> ${host_service_target}"
fi

echo "Reloading user systemd units"
systemctl --user daemon-reload
