---
name: webshot-general
description: General repository rules for the webshot repo (structure, where code/config/SQL/tests live, safety, and testing expectations). Use whenever making code changes in this repository.
---

# Webshot General

Use these rules whenever making code changes in this repository.

## Project overview
- Service is a web archive backend similar to archive.today.
- Main service sources, configs, and SQL live under `webshotd/`.
- Shared API and DTO source schemas live under repo-root `schema/`.
- Database bootstrap schemas live only in `webshotd/sql/schema/`.
- Tooling configs at repo root (`.clang-format`, `.editorconfig`, `.pre-commit-config.yaml`) define formatting and checks.

## Project structure
- `webshotd/src/` is the only allowed location for service `.cpp` sources; `webshotd/src/main.cpp` wires core components.
- Shared handlers/components headers MUST live in `webshotd/include/` (for example, `handler.hpp`, `crud.hpp`).
- Service SQL query files MUST live in `webshotd/sql/query/`.
- Service C++ unit tests and pytest tests MUST live under `webshotd/test/`.
- Root `test/` is reserved for repo-level fixtures and helper material; do not add service tests there.
- Chaotic input schemas live in `schema/*.yaml` and are generated into the build tree; edit the source schemas, not generated outputs.

## Testing expectations
- Pytest-based service and testsuite coverage lives under `webshotd/test/` and uses `pytest_userver` plus yandex-taxi-testsuite.
- When a change affects schemas, SQL, config, or request handling, update or add service tests in `webshotd/test/`.
