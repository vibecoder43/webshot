---
name: webshot-cpp
description: C++ coding rules for the webshot service (style, naming, safety constraints, and testing hygiene). Use when changing C++ code under `webshotd/` (including service sources, headers, and C++ tests).
---

# Webshot C++

Use these rules whenever making C++ changes in this repository.

## Language and headers
- C++20 is required to build the service; write code in C++17 unless specifically requested otherwise.
- All shared headers under `webshotd/include/` MUST use `#pragma once`.

## Style and naming
- Classes MUST use PascalCase (for example, `ByPrefixHandler`).
- Functions and variables MUST use lowerCamelCase.
- Constants MUST use the `kName` form.
- Default parameters in function declarations or definitions are forbidden.
- Namespace rules MUST be strictly followed: reuse the existing `namespace us = userver;` pattern where applicable, and use `::name` for global symbols.
- Use `{}` instead of `std::nullopt` in return statements and obvious initialization sites whenever it compiles.
- Use `size_t`, `int64_t` (not `std::size_t` or `std::int64_t`).
- Never use `Type name = Type(...)`; use `auto name = Type(...)` instead to avoid writing the type twice.
- Filenames MUST be snake_case (for example, `ip_utils.cpp`).
- Declarations and definitions MUST exactly match (names and signatures).
- Do not introduce duplicate code; factor common logic into reusable helpers.
- Code MUST be designed to handle adversarial input too.
- Prefer `std::begin`/`std::end` over calling `.begin()`/`.end()` on containers when passing iterators.
- Postfix arithmetic (`++`, `--`) MUST be used by default.
- Never set default values in code for component config options; require them in static config or config_vars.
- Class members must not use a trailing underscore naming style; use regular lowerCamelCase for member variables.
- Never call `std::chrono::system_clock::now()`; use `userver::utils::datetime::Now()` instead.
- Mutable lambdas are forbidden; capture-by-mutable is not allowed in this codebase.
- Catch-all exception handlers are forbidden; do not use `catch (...)`.
- Never use `return ReturnType(...)`; when constructing a value to return, prefer `return {...};` wherever it compiles.
- Never use `std::*stream*`; use `fmt` or userver I/O functionality.
- Never use `static_cast<IntType>`; use `numericCast` instead.

## [[nodiscard]] usage
- Do not annotate destructors, move operations, or obvious mutators.
- Favor annotating; compilers will surface accidental value drops.
- The `[[nodiscard]]` rules in this section are mandatory and MUST be followed strictly.

## Testing expectations
- C++ service tests use `userver::utest` and are wired from `webshotd/test/CMakeLists.txt`.
