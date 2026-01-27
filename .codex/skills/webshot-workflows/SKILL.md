---
name: webshot-workflows
description: Build, run, and test workflow for the webshot repo (devenv tasks, ctest, pytest).
---

# Webshot Workflows

Use this when the task involves building, running, or testing.

## Toolchain context
- C++20 is required to build the service (forced by `ada` and `userver`); write in C++17 unless specifically requested otherwise.
- The primary toolchain comes from `nix/toolchain.nix` and uses `pkgs.llvmPackages_21` (Clang 21 + matching `stdenv`).
- userver is consumed via the Nix flake in `nix/userver` (not by checking out userver sources in this repo).

## Agent sandbox limits
- `devenv` can be run by the agent, but it will likely need write escalation in sandboxed environments (it writes to `.devenv/*` and may write to out-of-repo runtime/cache dirs such as `$XDG_RUNTIME_DIR` like `/run/user/$UID`).

## Build, run, test
- Configure sanitizer build (Debug + ASan/UBSan) via `devenv tasks run webshot:configureSan` (binary dir `build/san`).
- Build service and tests after configuration via `devenv tasks run webshot:buildSan`.
- For release builds, use `devenv tasks run webshot:configureRelease` + `devenv tasks run webshot:buildRelease`.
- For coverage, use `devenv tasks run webshot:configureCov` + `devenv tasks run webshot:buildCov`.
- When passing config vars on the CLI, use `--config_vars` (underscore); userver does not support a `--config-vars` (dash) flag even if some upstream docs mention it.
- Run C++ tests in the sanitizer build directory with `ctest --output-on-failure` (usually from `build/san`).
- Python testsuite tests are run with `pytest` under the `tests/` directory and rely on `pytest_userver` plugins plus the `testsuite` Python package; do NOT add custom virtualenv creation here.
- The project uses the Ninja generator; there is no need to pass `-j` to `cmake --build` (parallelism is handled by Ninja or `--parallel` if needed).
