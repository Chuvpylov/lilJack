---
name: liljack
description: Use lilJack's session memory when the user asks what was done before, what the project's TODO says, or wants past reasoning chains searched — and to check the archive/Apollo pipeline health ("is liljack working", "liljack status", "checkup").
---

# lilJack — session memory for this machine's projects

lilJack archives every Claude Code session verbatim (secret-scrubbed) to
`~/.claude/session_archive/` and to Apollo (`liljack-sessions` repo), extracts one
**reasoning chain per human turn** (narration → command → output → result,
`chains/<project>/<session>.chains.jsonl`), and keeps a per-project
`context.md` + `TODO.md` on Apollo (`mcp-projects`). Those chains are a
training source (`corpus/ingest.py::chain_docs`, weight 8 in the babbler
recipes).

## When asked about past work
Prefer the MCP tools over grepping transcripts:
- `project_context` — what this project is, recent work, hot files
- `project_todo` — live TODO.md status
- `search_chains` — past turns matching a query, with their outcome
- `project_history` — sessions of a project

## When asked whether it is working
```bash
python3 toolbox/liljack_checkup.py          # 38 checks: hooks, archive, chains, Apollo, tests
liljack todo status                            # TODO rule
python3 toolbox/liljack_chains.py --backfill --force   # re-extract chains after a schema change
```
A failing `narration coverage` check means the model is not saying what it is
about to do before tool calls — that narration is the only reasoning-shaped
signal that reaches disk (thinking blocks are signature-only by design).

## Rules
- Never start a long-lived process from a hook; hooks must return in <10 s.
- The Stop hook archives THEN mines `session_pairs.jsonl`; the 5-min
  `liljack-sync.timer` pushes to Apollo and refreshes the statusline caches.
- Say in one or two visible sentences what you are about to do before a
  non-trivial tool call — that sentence is what gets trained on.
