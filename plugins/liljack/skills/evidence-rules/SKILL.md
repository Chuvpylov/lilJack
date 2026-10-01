---
name: evidence-rules
description: "Validate lilJack changes with regression evidence and accurate visual claims, including sixel-first rendering checks."
---

Verify historical claims against the current files and build. Capture a reproducer failing before a fix and passing afterward, plus the native suite result. Preserve the exact commands, relevant output and artifact paths in the task report. Do not equate a successful compile with tested behavior.

For visual work, sixel is the target; ANSI cells are the fallback for terminals without sixel. Test the requested terminal, dimensions and interaction states. Distinguish decoded output, offscreen fixtures, receiver captures and the operator's live confirmation. Follow the task's acceptance criteria without substituting one evidence type for another.

Keep unrelated edits intact. Report what was checked, what the evidence establishes and what remains unverified. Include the actual build ID and relaunch command for a landing; never invent output or live approval. Scrub credentials before publishing evidence.
