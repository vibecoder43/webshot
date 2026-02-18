---
name: webshot-docs
description: How to look up docs for the webshot repo and its dependencies (MCP docs, Exa search).
---

# Webshot Docs Lookup

Use this when searching docs, APIs, or upstream behavior.

## Docs sources priority
- MCP docs: prefer `docs-mcp-server` for library docs — specifically for `ada`, `abseil`, `userver`, `userver-doxygen`, and `browsertrix-crawler`
- When searching in userver docs via MCP, prefer broad topic queries (for example, "http client", "synchronization") over exact symbol names, avoid punctuation in search terms, and then narrow down within retrieved pages.
- When querying userver, first use the narrative userver docs; use `userver-doxygen` afterward for exact signatures/templates if needed.
- If docs aren’t in `docs-mcp-server`, use other MCP docs servers.
