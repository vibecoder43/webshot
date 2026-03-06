---
name: webshot-contracts
description: API/schema and change hygiene for the webshot repo (schemas, generated outputs, commits, PRs).
---

# Webshot Contracts

Use this when changing the HTTP API, schema, or preparing commits/PRs.

## API and schema alignment
- Public HTTP API is defined in `schema/webshot.yaml`; keep schemas, handlers, and generated DTOs aligned.
- Batch schema edits with their generated outputs.

## Commits and pull requests
- Follow `conventional_commits.md`; commit subjects use Conventional Commit style (such as `feat:` or `fix:`).
- Pull requests summarize behavior changes, list affected endpoints and configuration, link issues, and include relevant screenshots or schema diffs.
