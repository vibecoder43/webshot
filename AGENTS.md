# Repository Guidelines

## Project Structure
- `src/` is the only allowed location for service `.cpp` sources; `src/main.cpp` wires core components.
- Shared handlers/components MUST live in `include/` (for example, `webshot_handler.hpp`, `webshot_crud.hpp`), and all such headers MUST use `#pragma once`.
- Public HTTP API is defined in `schemas/webshot.yaml`; keep schemas and handlers aligned so generated serializers stay in sync.
- Database schemas MUST live only in `sql/schema/`.
- Tooling configs at repo root (`.clang-format`, `.editorconfig`, `.pre-commit-config.yaml`) define formatting and checks.
- MCP docs: prefer `docs-mcp-server` for library docs — specifically for `ada`, `abseil`, `userver`, `userver-doxygen`, and `browsertrix-crawler` — before web searching. When searching in userver docs via MCP, prefer broad topic queries (for example, \"http client\", \"synchronization\") over exact symbol names, avoid punctuation in search terms, and then narrow down within retrieved pages.
- When querying userver, first use the narrative userver docs; use `userver-doxygen` afterward for exact signatures/templates if needed.
- If docs aren’t in `docs-mcp-server`, use other MCP docs servers.
- Use Exa MCP web search before falling back to generic web search.

## Service & Agent Context
- Service is a web archive backend similar to archive.today.
- Agents can use build directories listed in `CMakePresets.json` (for example, `/tmp/build-webshot-san`) and local userver sources at `/home/noofva/misc/src/userver-develop`.

## Build, Run, Test
- Configure sanitizer build: `cmake --preset configure-preset-clang-san` (outputs to `/tmp/build-webshot-san`).
- Build and regenerate schema code: `cmake --build --preset build-preset-clang-san` after changing `schemas/`.
- When passing config vars on the CLI, use `--config_vars` (underscore); userver does not support a `--config-vars` (dash) flag even though some upstream docs mention it.
- Run tests: `ctest --output-on-failure` in the same build directory.
- The project uses the Ninja generator; there is no need to pass `-j` to `cmake --build` (parallelism is handled by Ninja or `--parallel` if needed).
- This is currently not supported due to unfinished integration with conan: Compute test coverage: configure with `cmake --preset configure-preset-clang-cov` (outputs to `/tmp/build-webshot-cov`, enables `WEBSHOT_ENABLE_COVERAGE=ON`), then `cmake --build --preset build-preset-clang-cov --target webshot-coverage` to build instrumented binaries, run tests, and emit llvm-cov output under `/tmp/build-webshot-cov/tests/coverage`; optional HTML rendering with `--target webshot-coverage-html` writes colored reports to `/tmp/build-webshot-cov/tests/coverage/html`.

## Style & Naming
- Target C++17; do not use C++20+ features.
- Classes MUST use PascalCase (for example, `WebshotCrud`).
- Functions and variables MUST use lowerCamelCase.
- Constants MUST use the `kName` form.
- Default parameters in function declarations or definitions are forbidden.
- Namespace rules MUST be strictly followed: reuse existing namespace aliases, and use `::name` for global symbols.
- Use `{}` instead of `std::nullopt` in return statements and obvious initialization sites whenever it compiles.
- Use `size_t`, `int64_t` (not `std::size_t` or `std::int64_t`).
- Never use `Type name = Type(...)`; use `auto name = Type(...)` instead to avoid writing the type twice.
- Filenames MUST be snake_case (for example, `ip_utils.cpp`, `webshot_handler.hpp`).
- Declarations and definitions MUST exactly match (names and signatures).
- Do not introduce duplicate code; factor common logic into reusable helpers.
- Code MUST be designed to handle adversarial input too.
- Prefer `std::begin`/`std::end` over calling `.begin()`/`.end()` on containers when passing iterators.
- Postfix arithmetic (`++`, `--`) MUST be used by default.
- Never set default values in code for component config options; require them in static config or config_vars.
- Do not introduce identifiers, filenames, configuration keys, environment variables, database objects, or documentation terms containing the words "application", "app", or "system".
- Class members must not use a trailing underscore naming style; use regular lowerCamelCase for member variables.
- Never call `std::chrono::system_clock::now()`; use `userver::utils::datetime::Now()` instead.
- Mutable lambdas are forbidden; capture-by-mutable is not allowed in this codebase.
- Never use `return ReturnType(...)`; when constructing a value to return, prefer `return {...};` wherever it compiles.

## [[nodiscard]] Usage
- Do not annotate destructors, move operations, or obvious mutators.
- Favor annotating; compilers will surface accidental value drops.
- The `[[nodiscard]]` rules in this section are mandatory and MUST be followed strictly.

## Testing
- All tests MUST live under `tests/` and MUST NOT be placed elsewhere.
- `userver::utest` (`<userver/utest/utest.hpp>`) is the only allowed framework for C++ unit tests so they run in coroutines with framework timeouts, logging, and sanitizers.

## Commits and Pull Requests
- Follow `conventional_commits.md`; commit subjects use Conventional Commit style (such as `feat:` or `fix:`).
- Batch schema edits with their generated outputs.
- Pull requests summarize behavior changes, list affected endpoints and configuration, link issues, and include relevant screenshots or schema diffs.

## Assistant Responses
- Do not respond with large blocks of code; show only short, focused snippets when necessary, or omit code entirely and describe the changes instead.

## Operational discipline
- Editing: make focused changes; avoid getting stuck on size/length.
- Verify, don’t recall: reread active code/logs; test exact endpoints/UI calls.
- Prefer authoritative data: no guessing or fallback to stale/synthetic for critical logic.
- Enforce encoding/syntax hygiene; avoid bulk pastes.
- Plan small, verify often; update docs immediately when behavior/contracts change.
- External libs: prefer valid codebases over ad-hoc experiments when directed.
- Scoping/edit size: timebox changes (5 min per edit), make an active decision if time expires.
- Always say what are the contents of your replies based on: memory, docs, something else.
