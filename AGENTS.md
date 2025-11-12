# Repository Guidelines

## Project Structure & Module Organization
- `src/` contains the userver service: `main.cpp` wires core components and `src/include/` exposes HTTP handlers such as `V1Files`.
- `schemas/openapi.yaml` defines the public API; CMake regenerates serializers into the build tree via `userver_target_generate_chaotic`, so keep schema and handler contracts aligned.
- `userver_config.yaml` configures listeners, task processors, and handler bindings; update it whenever you add components or change ports.
- Tooling configs at the repo root (`.clang-format`, `.editorconfig`, `.pre-commit-config.yaml`) govern formatting, editor defaults, and security checks.

## Service Overview
- Web archive backend modeled after archive.today but persisting each Browsertrix crawl as a WACZ package instead of HTML snapshots.
- Every capture is aliased with a UUIDv4; keep the identifier stable across API contracts, storage, and internal metadata.

## Agent Environment Notes
- ChatGPT agents can access every path declared in `CMakePresets.json`, including generated build directories such as `/tmp/build-webshot-san`; ensure sensitive data is kept elsewhere.
- Before editing, using, drawing conclusions from, or recalling the contents of any file, ChatGPT must re-check that the file has not changed since it was last read (e.g., by re-reading it). If the file has changed, base all actions on the latest contents.
- Local userver HTML docs are available at `/home/noofva/misc/src/userver-develop/build_san/docs/html/`.

## Build, Test, and Development Commands
- `cmake --preset configure-preset-clang-san` configures a sanitizer-enabled Ninja build in `/tmp/build-webshot-san` with the expected `userver_DIR`.
- `cmake --build --preset build-preset-clang-san` compiles targets and regenerates schema-derived code; rerun this after touching `schemas/`.
- `./webshot --config userver_config.yaml` (from the build directory) starts the service locally; override config paths with `--config` if you keep local variants.
- `ctest --output-on-failure` from the same build directory executes registered suites once tests are added.

## Coding Style & Naming Conventions
- Follow userver conventions: classes and components PascalCase (`V1Files`), constants prefixed `k`, functions lowerCamelCase, and request handlers suffixed with descriptive nouns.
- Keep shared headers under `src/include/` guarded by `#pragma once`; limit namespace aliases to implementation files unless broadly reused.

## Testing Guidelines
- Add unit or component tests under a `tests/` tree (create it if absent) and register each target in `CMakeLists.txt` with `add_executable` and `add_test`.
- Favor the `userver::utest` harness (see `<userver/utest/utest.hpp>`) so tests run inside coroutines and inherit the framework’s timeouts, logging, and sanitiser setup; only fall back to raw GoogleTest when migrating legacy code.
- Update `userver_config.yaml` or per-test configs to isolate external dependencies; aim for deterministic tests that fail fast on regressions.

## Commit & Pull Request Guidelines
- Adhere to the rules documented in `conventional_commits.md`; subjects stay in Conventional Commit form (`feat:`, `fix:`) with wrapped bodies for non-trivial context, and batch schema edits with their generated outputs.
- Pre-commit hooks run automatically; optionally run `pre-commit run --all-files` when you want to re-check locally or troubleshoot.
- Pull requests should summarize behavior changes, list affected endpoints/configuration, link issues, and attach screenshots or schema diffs when relevant.
