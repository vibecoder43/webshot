# Repository Guidelines

Detailed repository rules live in local Codex skills under `.codex/skills/`.

Reminder:
- Use `webshot-general` for any code changes.
- Use `webshot-cpp` for C++ code changes (service code in `webshotd/`).
- Use `webshot-workflows` for configure/build/run/test tasks, especially `devenv tasks run webshot:*`.
- Use `webshot-contracts` for `schema/*.yaml`, SQL/API contract changes, and commits/PRs.
- Use `webshot-docs` for repo-pinned dependency docs and upstream research.

# General rules
- Never implement backwards compatibility or silent fallbacks unless told to do so.
- Prefer failing hard over silent fallbacks.
- Never introduce environment variables unless told to do so.

# Response discipline
- Do not respond with large blocks of code; show only short, focused snippets when necessary, or omit code entirely and describe changes instead.
- Verify, don't recall: reread active code/logs; test exact endpoints.
- Prefer authoritative data: no guessing or fallback to stale/synthetic for critical logic.
- Always say what the contents of replies are based on (memory, repo code, docs, tool output).
