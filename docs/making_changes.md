# General guidelines

## Repo layout

- Service sources and runtime, config, and SQL: `webshotd/`
- Shared API and DTO schemas, source of truth: `schema/`
- Service tests, C++ and pytest: `webshotd/test/`
- Tooling and build orchestration: `devenv/`, `devenv.nix`, `devenv.yaml`

## Rules of engagement

- Do not implement backward compatibility or silent fallbacks.
- Do not introduce new environment variables.
