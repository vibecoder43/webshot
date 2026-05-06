---
name: webshot-cpp
description: C++ coding rules for the webshot service. Use when changing C++ code.
---

# C++

## Language and headers
- The standard is C++26.
- All headers must use `#pragma once`.

## Style and naming
- Default parameters in function declarations or definitions are forbidden.
- Use `::name` for global symbols.
- Prefer using `{}` instead of `std::nullopt`, or `std::nullopt` instead of type whenever it compiles.
- Use `size_t`, `int64_t`, not `std::size_t` or `std::int64_t`.
- Never use `Type name = Type(...)` or `auto name = Type{...}`; use `Type name{...}` instead to avoid writing the type twice.
- Always factor common logic into reusable helpers.
- Code must be designed to handle adversarial input too.
- Use `std::begin`/`std::end` on containers.
- Postfix arithmetic (`++`, `--`) must be used by default.
- Never set default values in code for component config options; require them in static config or config_vars.
- Mutable lambdas are forbidden; capture-by-mutable is not allowed in this codebase.
- Catch-all exception handlers are forbidden; do not use `catch (...)`.
- Exceptions are forbidden in new C++ code (no `throw`); use `Invariant` (and other fail-fast primitives already used in the codebase) instead.
- Prefer `return {...};` instead of `return Type{...}` wherever it compiles.
- Never use `static_cast<IntType>`; use `NumericCast` instead.
- Printable text MUST use `text::String` in APIs and data structures. Use `std::string` only for raw owned byte buffers, serialization/transport boundaries, or third-party interfaces that require it. Use `std::string_view` only for non-text byte views or raw protocol parsing, not for printable text APIs.

## APIs
- Direct syscalls and C stdlib calls require rare case-by-case justification.
- Non-userver I/O is forbidden.
- Never call `std::chrono::system_clock::now()`; use `userver::utils::datetime::Now()` instead.
- Standard library concurrency primitives are forbidden, except for lock_guard-style types, atomic.

## [[nodiscard]] usage
- Favor annotating; compilers will surface accidental value drops.

## Tests
- C++ service tests use `userver::utest` and are wired from `webshotd/test/CMakeLists.txt`.
