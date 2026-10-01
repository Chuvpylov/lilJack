"""Harness-aware, read-only lifecycle lookup for the exact lead session.

The delivery pump must never submit into a lead that is mid-turn, so it needs a
trustworthy idle signal. `room_delivery.native_state` hardcoded
`~/.codex/liljack/lifecycle.sqlite3`, which is why automatic wake-up worked only
for a Codex lead and stopped the moment leadership moved.

Two rules shape this module.

⚠ UNKNOWN IS NEVER IDLE. Every unrecognised event classifies as "unknown", never
"idle". The failure directions are not symmetric: mistaking busy for idle types
into a working agent and corrupts its turn, while mistaking idle for busy only
delays a notification. `SubagentStop` is the concrete trap — a subagent finishing
does not mean the lead finished, so it is busy.

⚠ A MISSING STORE IS REPORTED, NOT GUESSED. A harness with no lifecycle store
(a harness whose hooks do not call record_event) returns an explicit unknown
result with a reason. Callers must wait unless state is positively idle.

Paths mirror liljack_codex.state_root() and honour the same environment
overrides; state_root() itself reads a module-level HARNESS from the environment
and so cannot be asked about a harness other than the caller's own.
"""
from __future__ import annotations

import json
import os
import contextlib
import sqlite3
import time
from datetime import datetime
from pathlib import Path

__all__ = ["store_path", "classify", "lookup", "verify_submit",
           "IDLE", "BUSY", "ENDED", "UNKNOWN"]

IDLE, BUSY, ENDED, UNKNOWN = "idle", "busy", "ended", "unknown"

# Only a turn boundary is idle. Everything else that we recognise is busy.
_IDLE_EVENTS = {"Stop"}
_ENDED_EVENTS = {"SessionEnd"}
_BUSY_EVENTS = {
    "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse",
    "Notification", "PermissionRequest", "SubagentStart",
    "SubagentStop",          # a subagent finished; the LEAD has not.
    "PreCompact", "Interrupt",
}

# ⚠ BUSY IS A CLAIM WITH A SHELF LIFE. A wedged session looks EXACTLY like a
# working one: its last event just stops arriving, and `busy` is sticky, so the
# heartbeat leaves it alone forever — "busy, leave it alone" is the correct rule
# applied to a false premise. Measured 2026-09-10: codex read `busy` from a
# PostToolUse 837 MINUTES (14 hours) old while holding an open task, and nothing
# in the system could say so; the lead only noticed by reading ages by hand.
#
# Past this age a BUSY event stops being evidence of work and becomes evidence
# of nothing. It degrades to UNKNOWN, which is deliberately NOT idle — we do not
# start nudging a session that might genuinely be mid-call — but it is REPORTED,
# so a wedged agent is visible instead of silent. Two hours is far longer than
# any real tool call on this box and far shorter than the 14 hours it took a
# human to spot this one.
BUSY_STALE_S = float(os.environ.get("LILJACK_BUSY_STALE_S", 2 * 3600))

# ⚠ THE TURN-END SIGNAL IS LOSSY, AND THAT IS WHY AGENTS GO INVISIBLE. `Stop` is
# the only event that means "my turn is over", and it is simply not always
# emitted: measured 2026-09-10 across the three live stores, codex recorded 245
# UserPromptSubmit against 130 Stop, so ROUGHLY HALF ITS TURNS END WITH NO Stop
# AT ALL (claude 75%, opencode 66% — lossy everywhere, worst on codex). The last
# event then stays a PostToolUse, which classifies BUSY, and "busy, leave it
# alone" keeps that agent alone forever. codex sat like that from 05:52 to 19:53
# holding an open task; a direct message was what finally woke it.
#
# A TOOL BOUNDARY TELLS US WHICH KIND OF QUIET THIS IS, and the two cases are
# opposite:
#   PreToolUse  last  -> a tool is RUNNING. Quiet is expected; stay BUSY.
#   PostToolUse last  -> the tool RETURNED and nothing followed. During real
#                        work the next event lands almost immediately —
#                        measured on 5,082 codex gaps: median 1.8s, p90 22.3s,
#                        p99 79.8s, and only 0.3% exceed 120s. Minutes of
#                        silence after a PostToolUse means the turn is over.
# 300s is ~3.8x that p99, so a false "idle" is very unlikely, and it is 24x
# faster than waiting out BUSY_STALE_S — which is the difference between an
# agent being nudged after five minutes and never.
IDLE_AFTER_POST_S = float(os.environ.get("LILJACK_IDLE_AFTER_POST_S", 300))


def classify(event: str | None) -> str:
    if not event:
        return UNKNOWN
    if event in _IDLE_EVENTS:
        return IDLE
    if event in _ENDED_EVENTS:
        return ENDED
    if event in _BUSY_EVENTS:
        return BUSY
    return UNKNOWN


def store_path(harness: str | None) -> Path | None:
    """The lifecycle DB for one harness, or None when that harness has no store.

    Mirrors liljack_codex.state_root() per harness instead of per process.
    """
    h = (harness or "").strip().lower()
    if h == "opencode":
        root = os.environ.get("LILJACK_OPENCODE_STATE") or str(
            Path(os.environ.get("OPENCODE_HOME", str(Path.home() / ".opencode"))) / "liljack")
    elif h == "codex":
        root = os.environ.get("LILJACK_CODEX_STATE") or str(
            Path(os.environ.get("CODEX_HOME", str(Path.home() / ".codex"))) / "liljack")
    elif h == "claude":
        root = os.environ.get("LILJACK_CLAUDE_STATE") or str(
            Path(os.environ.get("CLAUDE_HOME", str(Path.home() / ".claude"))) / "liljack")
    else:
        return None          # a harness with no store yet: say so, never guess.
    return Path(root) / "lifecycle.sqlite3"


def _project_key(project) -> str:
    try:
        from liljack_workspace import project_key
        return project_key(project)
    except Exception:
        return os.path.realpath(str(project))


def _unknown(reason, code):
    # The event text also keeps old delivery displays informative; no native
    # identity or timestamp is invented on an unresolved lookup.
    return {'native': None, 'event': 'Unknown: ' + reason, 'age': None, 'at': None,
            'state': UNKNOWN, 'reason': reason, 'reason_code': code}


def _workspace_scope(target, project, harness, workspace_root):
    root = Path(workspace_root or os.environ.get(
        'LILJACK_WORKSPACE_ROOT', str(Path.home() / '.cache/liljack/workspace')))
    database = root / 'workspace.sqlite3'
    if not database.exists():
        return False, None  # legacy records still require cwd + exact target
    try:
        with contextlib.closing(sqlite3.connect(database.resolve().as_uri() + '?mode=ro', uri=True)) as db:
            row = db.execute('SELECT project,data FROM sessions WHERE id=?', (target,)).fetchone()
        if not row:
            return False, None
        if _project_key(row[0]) != project:
            return False, _unknown('Workspace session belongs to another project', 'project_mismatch')
        data = json.loads(row[1])
        if not isinstance(data, dict):
            raise ValueError('invalid session data')
        expected = {'codex': 'codex', 'claude': 'claude', 'deepseek': 'opencode'}.get(data.get('agent'))
        if expected and expected != harness:
            return False, _unknown('Workspace session belongs to another harness', 'harness_mismatch')
        return True, None
    except (sqlite3.Error, OSError, ValueError, TypeError):
        return False, _unknown('Workspace session mapping is unreadable', 'workspace_unreadable')


def lookup(target: str, project, harness: str | None, path=None, workspace_root=None) -> dict:
    """Latest lifecycle event for the workspace session `target`.

    Managed identity is scoped against the workspace before matching events by
    workspace_session; cwd is only a legacy fallback. Unknown results carry a
    reason and never qualify as idle. All access is read-only.
    """
    db_path = Path(path) if path else store_path(harness)
    if not target:
        return _unknown('Workspace session identity is missing', 'missing_target')
    if not db_path:
        return _unknown('No lifecycle store is configured for this harness', 'unsupported_harness')
    if not db_path.exists():
        return _unknown('Lifecycle store is missing', 'store_missing')
    key = _project_key(project)
    managed, problem = _workspace_scope(target, key, harness, workspace_root)
    if problem:
        return problem
    try:
        with contextlib.closing(sqlite3.connect(db_path.resolve().as_uri() + "?mode=ro", uri=True)) as db:
            # ⚠ No "newest N events" pre-filter. Scanning only a recent window means a
            # QUIET lead's mapping falls out from under it as soon as busy workers push
            # its last event past the window — the lead then reads as unknown forever,
            # which is exactly the lead that most needs waking. Match the session
            # directly and let SQLite do the work.
            sql = "SELECT session_id FROM events WHERE json_extract(payload,'$.workspace_session')=?"
            args = [target]
            if not managed:
                sql += ' AND cwd=?'
                args.append(key)
            row = db.execute(sql + ' ORDER BY seq DESC LIMIT 1', args).fetchone()
            if not row:
                return _unknown('No lifecycle event maps to this workspace session in this project', 'session_unmapped')
            row = db.execute(
                "SELECT session_id,event,recorded_at FROM events WHERE session_id=? ORDER BY seq DESC LIMIT 1",
                (row[0],)).fetchone()
    except (sqlite3.Error, ValueError, OSError):
        return _unknown('Lifecycle store or event payload is unreadable', 'store_unreadable')
    if not row:
        return _unknown('Mapped native session has no lifecycle events', 'events_missing')
    try:
        age = time.time() - datetime.fromisoformat(str(row[2]).replace("Z", "+00:00")).timestamp()
    except (ValueError, TypeError):
        return _unknown('Latest lifecycle timestamp is invalid', 'invalid_timestamp')
    state = classify(row[1])
    # ⚠ `age` WAS COMPUTED HERE AND NEVER USED. That is the whole defect: the
    # one number that could tell a working session from a wedged one was
    # measured, returned, and consulted by nobody.
    # A returned tool with nothing after it is a finished turn, not work.
    if state == BUSY and row[1] == "PostToolUse" and age > IDLE_AFTER_POST_S:
        return {"native": row[0], "event": row[1], "age": age, "at": row[2],
                "state": IDLE, 'reason': None, 'reason_code': 'turn_ended_no_stop'}
    if state == BUSY and age > BUSY_STALE_S:
        return {"native": row[0], "event": row[1], "age": age, "at": row[2],
                "state": UNKNOWN,
                'reason': f'No lifecycle event for {age/3600:.1f}h — session appears wedged',
                'reason_code': 'event_stale'}
    return {"native": row[0], "event": row[1], "age": age, "at": row[2],
            "state": state, 'reason': 'Unrecognized lifecycle event' if state == UNKNOWN else None,
            'reason_code': 'unknown_event' if state == UNKNOWN else None}


def verify_submit(target, project, harness, before, timeout=3.0, sleep=0.25,
                  workspace_root=None):
    """Classify what actually happened after a session submit.

    `before` is the lookup() dict captured BEFORE the Enter was pressed. Returns
    (outcome, detail) with outcome one of delivered / queued / unverified.

    ⚠ A submit that only claims "sent_to_tmux" proves nothing: tmux accepting a
    keystroke is not the same as the target TUI acting on it. the operator 2026-09-09:
    "codex instructions you pasted where not submitted while screen is in
    screensaver mode" — the keystroke was delivered but no turn started, and the
    caller had no way to tell.

    delivered  — a NEW lifecycle event landed (the submit started a turn).
    queued     — the target was already mid-turn and no new event landed: the
                 line is held for the next turn, not lost (the Codex TUI's
                 "messages to be submitted after next tool call").
    unverified — no lifecycle signal; the caller cannot claim delivery and must
                 not blindly re-submit (a queued line would double-send).
    """
    if not before:
        before = lookup(target, project, harness, workspace_root=workspace_root)
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(sleep)
        now = lookup(target, project, harness, workspace_root=workspace_root)
        if now.get("at") and now.get("at") != before.get("at"):
            return "delivered", f"turn started ({now.get('event') or 'new event'})"
        if now.get("state") == "unknown" and before.get("state") != "unknown":
            return "unverified", now.get("reason") or "lifecycle became unknown"
    if before.get("state") == "busy":
        return "queued", "target was mid-turn; line held for the next turn"
    return "unverified", f"no new lifecycle event within {timeout:.0f}s"
