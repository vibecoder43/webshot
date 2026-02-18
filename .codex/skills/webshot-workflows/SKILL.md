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

## Build, run, test
- Configure sanitizer build (Debug + ASan/UBSan) via `devenv tasks run webshot:configureSan` (binary dir `build/san`).
- Build service and tests after configuration via `devenv tasks run webshot:buildSan`.
- For release builds, use `devenv tasks run webshot:configureRelease` + `devenv tasks run webshot:buildRelease`.
- For coverage, use `devenv tasks run webshot:configureCov` + `devenv tasks run webshot:buildCov`.
- When passing config vars on the CLI, use `--config_vars` (underscore); userver does not support a `--config-vars` (dash) flag even if some upstream docs mention it.
- Run C++ tests in the sanitizer build directory with `ctest --output-on-failure` (usually from `build/san`).
