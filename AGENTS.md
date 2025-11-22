# Repository Guidelines

## Project Structure
- `src/` holds the userver service; `src/main.cpp` wires core components.
- Shared handlers/components live in `include/` (for example, `webshot_handler.hpp`, `webshot_crud.hpp`), headers use `#pragma once`.
- Public HTTP API is defined in `schemas/webshot.yaml`; keep schemas and handlers aligned so generated serializers stay in sync.
- Database schemas live in `sql/schema/`.
- Tooling configs at repo root (`.clang-format`, `.editorconfig`, `.pre-commit-config.yaml`) define formatting and checks.
- MCP docs: prefer `docs-mcp-server` for library docs — specifically for `ada`, `abseil`, `userver`, `userver-doxygen`, and `browsertrix-crawler` — before web searching.
- If docs aren’t in `docs-mcp-server`, use Context7 (`/authelia/authelia`, `/nginx/nginx`, etc.) before falling back to generic web search.
- When querying userver, first use the narrative userver docs; use `userver-doxygen` afterward for exact signatures/templates if needed.
- When searching in userver docs via MCP, prefer broad topic queries (for example, \"http client\", \"synchronization\") over exact symbol names, avoid punctuation in search terms, and then narrow down within retrieved pages.

## Service & Agent Context
- Service is a web archive backend similar to archive.today.
- Agents can use build directories listed in `CMakePresets.json` (for example, `/tmp/build-webshot-san`) and local userver docs at `/home/noofva/misc/src/userver-develop/build_san/docs/html/`.

## Build, Run, Test
- Configure sanitizer build: `cmake --preset configure-preset-clang-san` (outputs to `/tmp/build-webshot-san`).
- Build and regenerate schema code: `cmake --build --preset build-preset-clang-san` after changing `schemas/`.
- Run service from build directory: `./webshot --config userver_config.yaml` (override config path with `--config` for local variants).
- Run tests: `ctest --output-on-failure` in the same build directory.
- Compute test coverage: configure with `cmake --preset configure-preset-clang-cov` (outputs to `/tmp/build-webshot-cov`, enables `WEBSHOT_ENABLE_COVERAGE=ON`), then `cmake --build --preset build-preset-clang-cov --target webshot-coverage` to build instrumented binaries, run tests, and emit llvm-cov output under `/tmp/build-webshot-cov/tests/coverage`; optional HTML rendering with `--target webshot-coverage-html` writes colored reports to `/tmp/build-webshot-cov/tests/coverage/html`.
- `WebshotConfig` is excluded from coverage accounting and shouldn’t be targeted by tests.

## Style & Naming
- Target C++17; do not use C++20+ features.
- Classes: PascalCase (for example, `WebshotCrud`).
- Functions and variables: lowerCamelCase.
- Constants: `kName` form.
- Prefer `{}` instead of `std::nullopt` in return statements and obvious initialization sites when it clearly compiles.
- Use `size_t`, `int64_t` (not `std::size_t` or `std::int64_t`).
- Filenames in snake_case (for example, `ip_utils.cpp`, `webshot_handler.hpp`).
- Declarations must exactly match definitions (names and signatures).
- Avoid duplicate code; prefer reusable helpers.
- Do not introduce identifiers, filenames, configuration keys, environment variables, database objects, Docker labels, or documentation terms containing the words "application", "app", or "system".

## [[nodiscard]] Usage
- Prefer `[[nodiscard]]` on functions that return values that should not be ignored (such as `std::optional<T>`, containers or DTOs, find or query helpers, and JSON or HTTP helpers).
- Prefer `[[nodiscard]]` on lightweight data structs or classes that are commonly returned where dropping them is a bug (such as `Link`, `Webshot`, handler components).
- Do not annotate destructors, move operations, or obvious mutators.
- When unsure, favor annotating; compilers will surface accidental value drops.

## Testing
- Place tests under `tests/`.
- Use `userver::utest` (`<userver/utest/utest.hpp>`) so tests run in coroutines with framework timeouts, logging, and sanitizers.
- Register each test target in `CMakeLists.txt` with `add_executable` and `add_test`.
- Configure test `userver_config.yaml` variants to isolate external dependencies and keep tests deterministic.

## Commits and Pull Requests
- Follow `conventional_commits.md`; commit subjects use Conventional Commit style (such as `feat:` or `fix:`).
- Batch schema edits with their generated outputs.
- Pull requests summarize behavior changes, list affected endpoints and configuration, link issues, and include relevant screenshots or schema diffs.

## Assistant Responses
- Do not respond with large blocks of code; show only short, focused snippets when necessary, or omit code entirely and describe the changes instead.
