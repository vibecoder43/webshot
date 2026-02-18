---
name: webshot-core
description: Core repository rules for the webshot repo (structure, naming, safety, tests).
---

# Webshot Core

Use these rules whenever making code changes in this repository.

## Project overview
- Service is a web archive backend similar to archive.today (see `design.md` for behavior and data model details).
- Database schemas MUST live only in `sql/schema/`.
- Tooling configs at repo root (`.clang-format`, `.editorconfig`, `.pre-commit-config.yaml`) define formatting and checks.

## Project structure
- `src/` is the only allowed location for service `.cpp` sources; `src/main.cpp` wires core components.
- Shared handlers/components MUST live in `include/` (for example, `webshot_handler.hpp`, `webshot_crud.hpp`), and all such headers MUST use `#pragma once`.
- C++ unit tests MUST live under `tests/` and use `userver::utest` (`<userver/utest/utest.hpp>`).
- Functional tests and testsuite helpers also live under `tests/` and are driven by `pytest`, `pytest_userver`, and `testsuite` (from yandex-taxi-testsuite).  All tests MUST live under `tests/` and MUST NOT be placed elsewhere.

## Style and naming
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

## [[nodiscard]] usage
- Do not annotate destructors, move operations, or obvious mutators.
- Favor annotating; compilers will surface accidental value drops.
- The `[[nodiscard]]` rules in this section are mandatory and MUST be followed strictly.
