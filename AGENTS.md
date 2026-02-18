# Repository Guidelines

Detailed repository rules live in local Codex skills under `.codex/skills/`.

Reminder:
- Use `webshot-core` for any code changes.
- Use `webshot-workflows` for build/run/test tasks.
- Use `webshot-contracts` for API/schema/DTO changes and commits/PRs.
- Use `webshot-docs` for docs lookup and upstream research.

# Response discipline
- Never implement backwards compatibility or silent fallbacks unless told to do so.
- Do not respond with large blocks of code; show only short, focused snippets when necessary, or omit code entirely and describe changes instead.
- Verify, don't recall: reread active code/logs; test exact endpoints.
- Prefer authoritative data: no guessing or fallback to stale/synthetic for critical logic.
- Always say what the contents of replies are based on (memory, repo code, docs, tool output).

# Text hygiene
- Keep tracked source/config/docs ASCII-only; encode any required Unicode as escapes (e.g. `\uXXXX`, `\xNN`, `\ooo`).
- To scan: `git ls-files -z | xargs -0 rg -n --pcre2 '[^\x00-\x7F]'`
