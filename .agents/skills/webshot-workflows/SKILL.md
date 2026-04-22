---
name: webshot-workflows
description: Build, run, and test workflow for the webshot repo
---

# Webshot Workflows

Use this when the task involves building, running, or testing.

## Toolchain context
- C++26 is required to build the service.
- The primary toolchain comes from `devenv/toolchain.nix` and uses `pkgs.llvmPackages_22` (Clang 22 + matching `stdenv`).
- userver source comes from the flake inputs and is packaged via `devenv/pkgs/userver.nix`.
- CMake configures `./webshotd` with Ninja into `build/webshotd/{san,tidy,cov,release}`.
- Don not pass -j N arguments to CMake or Ninja.

## Agent sandbox limits
- `devenv` can be run by the agent, but it will likely need write escalation in sandboxed environments.
- If a command fails due to permissions, rerun it with escalation rather than inventing an alternate workflow.

## Preferred tasks
- Build the default sanitizer/dev variant with `devenv tasks run proj:devBuild`.
- Bring the dev stack up with `devenv tasks run proj:devUp`, tear it down with `devenv tasks run proj:devDown`, inspect it with `devenv tasks run proj:devStatus`, and stream logs with `devenv tasks run proj:devLogs`.
- Run the service test flow with `devenv tasks run proj:devTest`.
- Use the prodlike stack tasks when needed: `devenv tasks run proj:prodlikeBuild`, `devenv tasks run proj:prodlikeUp`, `devenv tasks run proj:prodlikeDown`, `devenv tasks run proj:prodlikeStatus`, `devenv tasks run proj:prodlikeLogs`.

## Runtime and test details
- The task wrappers invoke `python3 -m s6.runtime` and source config vars from `webshotd/config/config_vars.dev.yaml` or `webshotd/config/config_vars.prodlike.yaml`.
