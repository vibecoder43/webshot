# Workflows: build, run, test

This repo is set up to be built and run via `devenv` tasks.

## Toolchain context

- Toolchain is driven by Nix and `devenv`. See `devenv tasks list`.
- Clang comes from `devenv/toolchain.nix`, `llvmPackages_22`.
- CMake configures `./webshotd` with Ninja into build dirs under `build/webshotd/`.

## Preferred tasks

Build:

- `devenv tasks run proj:devBuild`
- `devenv tasks run proj:prodlikeBuild`
- `devenv tasks run proj:tidyBuild`

Run the stack:

- `devenv tasks run proj:devUp`
- `devenv tasks run proj:devDown`
- `devenv tasks run proj:devStatus`
- `devenv tasks run proj:devLogs`

Test:

- `devenv tasks run proj:devTest`
- `devenv tasks run proj:devTestFailFast`

DB tasks:

- `devenv tasks run proj:devDbMigrate`
- `devenv tasks run proj:devDbBaseline`
...

## Runtime notes

- Dev mode uses a single state directory under `/tmp/webshot/dev`; see `devenv/tasks.nix`.
