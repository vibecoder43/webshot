---
name: webshot-docs
description: How to look up docs for the webshot repo and its dependencies
---

# Webshot Docs Lookup

Use this when searching docs, APIs, or upstream behavior.

## Docs sources priority
- Start with repo-pinned sources for version/context: `devenv.yaml`, `devenv.lock`, `nix/toolchain.nix`, `devenv/*.nix`, and `webshotd/CMakeLists.txt`.
- Repo code and schemas are authoritative for local behavior: use `webshotd/config/`, `webshotd/sql/`, `webshotd/test/`, and `schema/*.yaml` before assuming upstream defaults.
- MCP docs: prefer `docs-mcp-server` for libraries used here, especially `userver`, `userver-doxygen`, `ada`, `abseil`, and `browsertrix-crawler`.
- When searching in userver docs via MCP, prefer broad topic queries (for example, "http client", "synchronization") over exact symbol names, avoid punctuation in search terms, and then narrow down within retrieved pages.
