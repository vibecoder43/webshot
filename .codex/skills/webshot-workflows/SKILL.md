---
name: webshot-workflows
description: Build, run, and test workflow for the webshot repo
---

# Webshot Workflows

Use this when the task involves building, running, or testing.

## Toolchain context
- C++20 is required to build the service (forced by `ada` and `userver`); write in C++17 unless specifically requested otherwise.
- The primary toolchain comes from `nix/toolchain.nix` and uses `pkgs.llvmPackages_21` (Clang 21 + matching `stdenv`).
- userver is consumed via the Nix flake in `nix/userver` (not by checking out userver sources in this repo).

## Agent sandbox limits
- `devenv` can be run by the agent, but it will likely need write escalation in sandboxed environments.
- If a command fails due to permissions, try running devenv shell -- command

## Build, run, test
- Build service and tests after configuration via `devenv tasks run webshot:buildSan`.
- When passing config vars on the CLI, use `--config_vars` (underscore); userver does not support a `--config-vars` (dash) flag even if some upstream docs mention it.
- Test the project using `devenv tasks run webshot:testSan`. Trying other ways is unlikely to succeed.
