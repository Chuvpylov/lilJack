#!/usr/bin/env python3
"""claude lifecycle adapter — records Claude Code's turn lifecycle events into
its own store so a claude lead emits a usable idle signal, like codex/opencode.

Records the lifecycle event ONLY. Claude Code's own archive (session_archive.py
--stop) stays the owner of transcript archiving, so this adapter never passes
transcript_path to record_event — no archive_queue entry, no rollout snapshot,
no sync worker.

The event name is authoritative from argv[1] (the settings.json hook command
knows which event it is for); it falls back to stdin's hook_event_name only when
no argument is given. An event is recorded ONLY if it is a known lifecycle
event, so a busy event can never be mislabelled "Stop" (idle) and a new
UserPromptSubmit/PreToolUse naturally supersedes the previous Stop.

The environment is forced before importing liljack_codex because that module
reads LILJACK_HARNESS / LILJACK_AGENT at import time. Unconditional, so it
survives an operator shell that exported LILJACK_HARNESS=opencode.
"""
import json
import os
import sys
from pathlib import Path

os.environ["LILJACK_HARNESS"] = "claude"
os.environ.setdefault("LILJACK_AGENT", "claude")

sys.path.insert(0, str(Path(__file__).resolve().parent))

import liljack_codex as _codex  # noqa: E402  (reads the env above at import)


def main():
    event = sys.argv[1] if len(sys.argv) > 1 else None
    data = {}
    try:
        loaded = json.load(sys.stdin)
        if isinstance(loaded, dict):
            data = loaded
    except Exception:
        pass
    if not event:
        event = data.get("hook_event_name")
    if not isinstance(event, str) or event not in _codex.EVENTS:
        return 0
    sid = data.get("session_id")
    cwd = data.get("cwd")
    if not isinstance(sid, str) or not sid or not isinstance(cwd, str) or not cwd:
        return 0
    try:
        _codex.record_event({"hook_event_name": event, "session_id": sid, "cwd": cwd})
    except Exception:
        pass
    print("{}")
    return 0


if __name__ == "__main__":
    main()
