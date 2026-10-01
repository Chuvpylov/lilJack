---
name: report-format
description: "Write a lilJack task landing report with checked evidence, verification limits, and relaunch instructions."
---

Write the task report under docs/codex/reports/ in the applicable project. Include the task and exact room ID, what was checked, findings with file/line or artifact evidence, and what was NOT verified.

For every landing include fail-before/pass-after test output, the native suite result line, the report path, and a relaunch command with the actual build ID. Distinguish a proposed command from an executed relaunch. If a check cannot run, record the reason rather than presenting it as passing. Do not claim interactive confirmation from filesystem checks or screenshots from code inspection.

Post the report and relevant evidence to the exact room, update your own registry row according to actual acceptance, then stop that task. Scrub credentials; never print a ring token or credential-shaped value. A stopped session does not establish completion.
