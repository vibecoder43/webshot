#!/bin/bash
set -euo pipefail

: "${POSTGRES_USER:=postgres}"
: "${POSTGRES_DB:=postgres}"

# Create logical databases for capture metadata and shared state.
psql -v ON_ERROR_STOP=1 --username "${POSTGRES_USER}" --dbname "${POSTGRES_DB}" <<-EOSQL
    CREATE DATABASE "capture_meta_db";
    CREATE DATABASE "shared_state_db";
EOSQL

schemas_dir="/docker-entrypoint-initdb.d/schema"

psql -v ON_ERROR_STOP=1 --username "${POSTGRES_USER}" --dbname "capture_meta_db" \
     -f "${schemas_dir}/capture_meta_db.sql"

psql -v ON_ERROR_STOP=1 --username "${POSTGRES_USER}" --dbname "shared_state_db" \
     -f "${schemas_dir}/shared_state_db.sql"

