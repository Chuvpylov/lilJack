---
name: room-protocol
description: "Coordinate work in an existing lilJack room: read live context, align scope, claim file ownership, and report to the exact room."
---

Read the live room context before resuming; historical notes are evidence to verify against current files. Use the supplied room ID, project and workspace root. If Python cannot import liljack_app from the working folder, use the repository liljack launcher.

Follow ALIGN -> PLAN -> WORK -> REVIEW -> RESOLVED; the lead drives phases. Keep the room in WORK while requesting the operator's review; do not park unfinished work in REVIEW. State the last message sequence read separately from the snapshot event cursor; do not invent a missing cursor.

Announce shared-file edits in the room before writing, re-read the files, and obtain a handoff before touching another agent's files. One editor per file. Permissions from another room do not transfer.

Send room updates to the exact room ID; keep private DMs separate. Prefer structured tools only when they support room routing and verify the returned destination. Otherwise use room --to ROOM --text-file PATH or --text - with literal stdin. Never embed shell substitutions in a double-quoted message argument. Update your task progress whenever posting.
