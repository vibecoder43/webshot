# C++ guidelines

## Language

- C++ standard is C++26, see `webshotd/CMakeLists.txt`
- All headers use `#pragma once`.

## Style and safety

- Default parameters in function declarations or definitions are forbidden.
- Use `{}` initialization by default; avoid repeating the type name, including return type.
- Use `size_t` and `int64_t`, not `std::size_t` and `std::int64_t`.
- Use postfix `++` and `--` by default.
- Factor common logic into helpers.
- Design for adversarial input.

## API constraints

- Exceptions are forbidden in new code; `throw` is not allowed, and catch-all exception handlers are not allowed. Use existing fail-fast primitives such as `Invariant`.
- Standard library concurrency primitives are forbidden, except lock-guard style types and atomics.
- Do not call `std::chrono::system_clock::now`; use `userver::utils::datetime::Now`.
- No direct syscalls and C stdlib calls; non-userver I/O is not allowed.

## Text and casting

- Printable text in APIs and data structures uses `text::String`.
  - Use `std::string` for bytes, serialization boundaries, required third-party interfaces.
  - Use `std::string_view` for non-text byte views or protocol parsing.
- Do not use `static_cast<IntType>` for numeric casts; use `NumericCast`.

## Configuration

- Do not set default values in C++ code for component config options. Require them in static config or config vars.

## Tests

- C++ unit tests use `userver::utest` and are wired from `webshotd/test/CMakeLists.txt`.
