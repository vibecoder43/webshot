---
name: webshot-workflows
description: Build, run, and test workflow for the webshot repo
---

# Webshot Workflows

Use this when the task involves building, running, or testing.

## Toolchain context
- C++20 is required to build the service (forced by upstream dependencies).
- The primary toolchain comes from `nix/toolchain.nix` and uses `pkgs.llvmPackages_21` (Clang 21 + matching `stdenv`).
- userver is consumed via the Nix flake in `nix/userver` (not by checking out userver sources in this repo).
- CMake configures `./webshotd` with Ninja into `build/webshotd/{san,tidy,cov,release}`.

## Agent sandbox limits
- `devenv` can be run by the agent, but it will likely need write escalation in sandboxed environments.
- If a command fails due to permissions, rerun it with escalation rather than inventing an alternate workflow.

## Preferred tasks
- Build the default sanitizer/dev variant with `devenv tasks run webshot:devBuild`.
- Bring the dev stack up with `devenv tasks run webshot:devUp`, tear it down with `webshot:devDown`, inspect it with `webshot:devStatus`, and stream logs with `webshot:devLogs`.
- Run the service test flow with `devenv tasks run webshot:devTest`.
- Use the prodlike stack tasks when needed: `webshot:prodlikeBuild`, `webshot:prodlikeUp`, `webshot:prodlikeDown`, `webshot:prodlikeStatus`, `webshot:prodlikeLogs`.

## Runtime and test details
- The task wrappers invoke `python3 -m s6.runtime` and source config vars from `webshotd/config/config_vars.dev.yaml` or `webshotd/config/config_vars.prodlike.yaml`.
- `webshot:devTest` builds, starts the `test_infra` profile, exports the runtime `LD_LIBRARY_PATH`, and runs `ctest` from `build/webshotd/san`.
- The dev shell exports `USERVER_DIR`, `USERVER_PYTHON`, `PYTHONPATH`, and helper commands such as `webshot_test_san` and `webshot_test_cov`.
- If you must configure or build manually, point CMake at `-S ./webshotd`; do not assume a repo-root `CMakeLists.txt`.

## Binary invocation
- When passing config vars on the CLI, use `--config_vars` (underscore); userver does not support a `--config-vars` (dash) flag even if some upstream docs mention it.
- The service binary and runtime scripts also use `--config_vars_override` with underscores.
