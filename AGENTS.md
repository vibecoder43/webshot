# Repository Guidelines

## Project Structure & Module Organization
- `src/` contains the userver service: `src/main.cpp` wires core components and `src/include/` exposes HTTP handlers and components such as `webshot_handler.hpp`, `webshot_crud.hpp`, and friends.
- `schemas/webshot.yaml` defines the public API; CMake regenerates serializers into the build tree via `userver_target_generate_chaotic`, so keep schema and handler contracts aligned.
- `userver_config.yaml` configures listeners, task processors, and handler bindings; update it whenever you add components or change ports.
- `sql/schema/` holds Postgres schema files for the metadata and denylist databases used by the service.
- Tooling configs at the repo root (`.clang-format`, `.editorconfig`, `.pre-commit-config.yaml`) govern formatting, editor defaults, and security checks.

## Service Overview
- Web archive backend modeled after archive.today but persisting each Browsertrix crawl as a WACZ package instead of HTML snapshots.
- Every capture is aliased with a UUIDv4; keep the identifier stable across API contracts, storage, and internal metadata.

## Agent Environment Notes
- ChatGPT agents can access every path declared in `CMakePresets.json`, including generated build directories such as `/tmp/build-webshot-san`; ensure sensitive data is kept elsewhere.
- Local userver HTML docs are available at `/home/noofva/misc/src/userver-develop/build_san/docs/html/`.

## Build, Test, and Development Commands
- `cmake --preset configure-preset-clang-san` configures a sanitizer-enabled Ninja build in `/tmp/build-webshot-san` with the expected `userver_DIR`.
- `cmake --build --preset build-preset-clang-san` compiles targets and regenerates schema-derived code; rerun this after touching `schemas/`.
- `./webshot --config userver_config.yaml` (from the build directory) starts the service locally; override config paths with `--config` if you keep local variants.
- `ctest --output-on-failure` from the same build directory executes registered suites once tests are added.

## Coding Style & Naming Conventions
- Follow userver conventions: classes and components PascalCase (`V1Files`), constants prefixed `k`, functions lowerCamelCase, and request handlers suffixed with descriptive nouns.
- Object (variable) names are lowerCamelCase.
- Write `size_t`, `ssize_t`, `int64_t` (not `std::size_t`/`std::ssize_t`/`std::int64_t`).
- Keep shared headers under `src/include/` guarded by `#pragma once`; limit namespace aliases to implementation files unless broadly reused.
- Declarations must exactly match definitions (names and signatures).
- Use parentheses for initialization, not brace init (except where aggregate-init is required).
- Filenames must be snake_case (e.g., `ip_utils.cpp`, `webshot_handler.hpp`).
- Naming restriction: never introduce identifiers, filenames, configuration keys, env vars, database objects, Docker labels, or documentation terms that include the words "application", "app", or "system".
- Avoid writing duplicate code when a reusable function can help, even if that requires adding functions, files, etc.

### [[nodiscard]] usage
- Prefer `[[nodiscard]]` on any function that returns a value that should not be ignored (e.g., `std::optional<T>`, containers/DTOs, builders, find/query helpers, and JSON/HTTP response helpers).
- Prefer `[[nodiscard]]` on lightweight data structs/classes that are commonly returned from functions where dropping the value is a bug (e.g., `Link`, `Webshot`, handler components).
- Do not annotate destructors, move operations, or obvious mutators/setters.
- If a function returns only for side-effects (logging, metrics, void), do not mark it `[[nodiscard]]`.
- When in doubt, annotate; the compiler will surface accidental value drops during reviews.

## Language Standard
- The codebase targets C++17 semantics, regardless of any higher standard values set in `CMakeLists.txt` for dependencies or tooling. Do not use C++20+ language/library features in project code.

## Testing Guidelines
- Add unit or component tests under a `tests/` tree (create it if absent) and register each target in `CMakeLists.txt` with `add_executable` and `add_test`.
- Favor the `userver::utest` harness (see `<userver/utest/utest.hpp>`) so tests run inside coroutines and inherit the framework’s timeouts, logging, and sanitiser setup;
- Update `userver_config.yaml` or per-test configs to isolate external dependencies; aim for deterministic tests that fail fast on regressions.

## Commit & Pull Request Guidelines
- Adhere to the rules documented in `conventional_commits.md`; subjects stay in Conventional Commit form (`feat:`, `fix:`) with wrapped bodies for non-trivial context, and batch schema edits with their generated outputs.
- Pull requests should summarize behavior changes, list affected endpoints/configuration, link issues, and attach screenshots or schema diffs when relevant.
