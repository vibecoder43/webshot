# Contracts: API, schemas, SQL

Use this when changing HTTP APIs, schema, SQL, or when preparing commits.

## HTTP API and DTO schemas

Source-of-truth schemas live under `schema/`:

- Public API: `schema/public/webshot.yaml`
- Admin API: `schema/admin/webshot.yaml`
- Shared DTOs: `schema/common/common.yaml`
- Additional DTO sources: `schema/cdp.yaml`, `schema/browsertrix_pages.yaml`, `schema/wacz.yaml`

DTOs are generated into the build tree via `userver_target_generate_chaotic` from `webshotd/CMakeLists.txt`.

When a schema changes, keep these aligned in the same change:

- schema source files
- handler and business logic
- SQL shapes, when relevant
- service tests

## SQL contracts

- Database bootstrap schemas live under:
  - `webshotd/sql/schema/capture_meta_db/`
  - `webshotd/sql/schema/shared_state_db/`
- Query contracts live in `webshotd/sql/query/*.sql`.
