#!/usr/bin/env python3
"""Codex lifecycle adapter. Local capture, separate from Claude's archive/cache.

Hooks are the event interface. Rollout metadata is optional and version-sensitive;
never infer lifecycle completion from the last assistant message in a rollout.
"""
import argparse
from contextlib import closing
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import sys

EVENTS = {
    "SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse",
    "PostToolUse", "PermissionRequest", "PreCompact", "PostCompact",
    "SubagentStart", "SubagentStop", "Stop", "Interrupt",
}
CONTEXT_EVENTS = {"SessionStart", "UserPromptSubmit"}
SNAPSHOT_EVENTS = {"SessionStart", "Stop", "Interrupt", "PostCompact", "SessionEnd"}
HOOK_FIELDS = (
    "session_id", "turn_id", "cwd", "model", "permission_mode", "source",
    "trigger", "reason", "agent_id", "agent_type", "tool_name", "tool_use_id",
    "stop_hook_active",
)
META_FIELDS = ("id", "session_id", "timestamp", "cwd", "originator", "cli_version",
               "model_provider", "history_mode", "context_window")
TURN_FIELDS = ("turn_id", "root_turn_id", "cwd", "model", "effort",
               "approval_policy", "approvals_reviewer", "active_permission_profile",
               "summary", "timezone")
EVENT_MOODS = {
    "SessionStart": ("curious", "context loaded"),
    "UserPromptSubmit": ("focused", "eyes on the target"),
    "PreToolUse": ("working", "doing the thing"),
    "PostToolUse": ("working", "tool returned; verify the result"),
    "PermissionRequest": ("contemplative", "waiting for your call"),
    "PreCompact": ("contemplative", "packing the useful bits"),
    "PostCompact": ("focused", "context packed, thread intact"),
    "SubagentStart": ("scheming", "a helper joined the job"),
    "SubagentStop": ("contemplative", "helper returned; check the handoff"),
    "Stop": ("neutral", "turn parked; no victory lap assumed"),
    "Interrupt": ("contemplative", "paused, keeping the thread"),
    "SessionEnd": ("tired", "notes saved, lights down"),
}


HARNESS = os.environ.get("LILJACK_HARNESS", "codex")
AGENT = os.environ.get("LILJACK_AGENT", HARNESS)   # mailbox/board identity; may differ from HARNESS


def state_root():
    if HARNESS == "opencode":
        return Path(os.environ.get("LILJACK_OPENCODE_STATE", str(
            Path(os.environ.get("OPENCODE_HOME", str(Path.home() / ".opencode"))) / "liljack")))
    if HARNESS == "claude":
        return Path(os.environ.get("LILJACK_CLAUDE_STATE", str(
            Path(os.environ.get("CLAUDE_HOME", str(Path.home() / ".claude"))) / "liljack")))
    return Path(os.environ.get("LILJACK_CODEX_STATE", str(
        Path(os.environ.get("CODEX_HOME", str(Path.home() / ".codex"))) / "liljack")))


def _selected(data, keys):
    return {k: data[k] for k in keys if k in data and
            isinstance(data[k], (str, bool, int, float, type(None)))}


def rollout_metadata(path, expected_session="", tail_bytes=512 * 1024):
    """Read a bounded first-line/tail sample; skip messages and opaque reasoning.

    No writes to Codex-owned files. A missing field means unknown, not zero.
    The tail may omit older context/compaction records, so it isn't a full census.
    """
    if not path:
        return {"availability": "no_transcript"}
    try:
        with Path(path).open("rb") as stream:
            first = stream.readline(256 * 1024)
            header = json.loads(first)
            if not isinstance(header, dict) or header.get("type") != "session_meta":
                return {"availability": "unrecognized_format"}
            payload = header.get("payload")
            if not isinstance(payload, dict):
                return {"availability": "unrecognized_format"}
            sid = payload.get("session_id") or payload.get("id")
            if expected_session and sid != expected_session:
                return {"availability": "session_mismatch"}
            end = stream.seek(0, 2)
            start = max(len(first), end - tail_bytes)
            stream.seek(start)
            if start > len(first):
                stream.readline()  # partial first tail record
            tail = stream.read(tail_bytes)
    except (OSError, ValueError):
        return {"availability": "unreadable"}
    result = {"availability": "sampled", "sample_bytes": len(first) + len(tail),
              "tail_truncated": start > len(first),
              "session": _selected(payload, META_FIELDS)}
    source = payload.get("source", payload.get("thread_source"))
    result["session"]["source_kind"] = source if isinstance(source, str) else (
        sorted(source) if isinstance(source, dict) else None)
    if isinstance(payload.get("git"), dict):
        result["session"]["git"] = _selected(payload["git"], ("commit_hash", "branch"))
    for line in tail.splitlines(keepends=True):
        if not line.endswith(b"\n"):
            continue  # an active writer may not have finished this record
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if not isinstance(rec, dict) or not isinstance(rec.get("payload"), dict):
            continue
        p = rec["payload"]
        if rec.get("type") == "turn_context":
            result["turn"] = _selected(p, TURN_FIELDS)
            for key in ("sandbox_policy", "collaboration_mode"):
                if isinstance(p.get(key), dict):
                    result["turn"][key] = _selected(p[key], ("type", "mode"))
        elif rec.get("type") == "event_msg" and p.get("type") == "token_count":
            info = p.get("info")
            if isinstance(info, dict):
                usage = _selected(info, ("model_context_window",))
                for key in ("total_token_usage", "last_token_usage"):
                    if isinstance(info.get(key), dict):
                        usage[key] = _selected(info[key], (
                            "input_tokens", "cached_input_tokens", "output_tokens",
                            "reasoning_output_tokens", "total_tokens"))
                result["token_usage"] = usage
        elif rec.get("type") == "compacted":
            result["compaction_seen_in_sample"] = True
    return result


def _connect(root, write=False):
    path = Path(root) / "lifecycle.sqlite3"
    if write:
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        db = sqlite3.connect(path, timeout=3)
        path.chmod(0o600)
        db.executescript("""
            CREATE TABLE IF NOT EXISTS events (
                seq INTEGER PRIMARY KEY, recorded_at TEXT NOT NULL,
                session_id TEXT NOT NULL, cwd TEXT NOT NULL,
                event TEXT NOT NULL, payload TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS events_session ON events(session_id, seq);
            CREATE INDEX IF NOT EXISTS events_cwd ON events(cwd, seq);
            CREATE TABLE IF NOT EXISTS context_cache (
                session_id TEXT PRIMARY KEY, digest TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS archive_queue (
                session_id TEXT PRIMARY KEY, cwd TEXT NOT NULL,
                transcript TEXT NOT NULL, revision TEXT NOT NULL);
        """)
    else:
        if not path.exists():
            return None
        db = sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True, timeout=3)
    return db


def _capture_text(text):
    """Reuse Liljack's credential scrubber; omit text when it is unavailable."""
    if not isinstance(text, str) or not text:
        return ""
    try:
        from liljack_chains import _redact_live_secrets
        cleaned, count = _redact_live_secrets(text)
        if count < 0:
            return "[text omitted: credential scrubber unavailable]"
        return cleaned[:8000]
    except Exception:
        return "[text omitted: credential scrubber unavailable]"


def record_event(data, root=None, scrub=_capture_text):
    event = data.get("hook_event_name")
    sid, cwd = data.get("session_id"), data.get("cwd")
    if event not in EVENTS or not isinstance(sid, str) or not sid or not isinstance(cwd, str) or not cwd:
        return None
    payload = _selected(data, HOOK_FIELDS)
    payload.update(harness=HARNESS, schema_version=1, hook_event_name=event)
    payload["cwd"] = str(Path(cwd).resolve())
    workspace_session = os.environ.get("LILJACK_WORKSPACE_SESSION", "")
    if workspace_session.startswith("s-") and not data.get("agent_id"):
        payload["workspace_session"] = workspace_session
    for key in ("prompt", "last_assistant_message"):
        if isinstance(data.get(key), str):
            payload[key] = scrub(data[key])
    if event in SNAPSHOT_EVENTS:
        payload["rollout"] = rollout_metadata(data.get("transcript_path"), sid)
    with closing(_connect(root or state_root(), write=True)) as db, db:
        db.execute("INSERT INTO events(recorded_at,session_id,cwd,event,payload) VALUES(?,?,?,?,?)",
                   (datetime.now(timezone.utc).isoformat(), sid, payload["cwd"], event,
                    json.dumps(payload)))
        if event in {"Stop", "Interrupt", "PreCompact", "SessionEnd", "SubagentStop"}:
            transcript = data.get("agent_transcript_path") if event == "SubagentStop" else data.get("transcript_path")
            if isinstance(transcript, str) and transcript:
                # SubagentStop's session_id is the parent, not the child transcript ID.
                actual_sid = sid
                if event == "SubagentStop":
                    child = rollout_metadata(transcript)
                    actual_sid = child.get("session", {}).get("session_id") or child.get("session", {}).get("id")
                if actual_sid:
                    db.execute("INSERT OR REPLACE INTO archive_queue VALUES(?,?,?,?)",
                               (actual_sid, payload["cwd"], transcript,
                                datetime.now(timezone.utc).isoformat()))
    return payload


def history(cwd="", session_id="", limit=20, query="", root=None):
    """Project-scoped event history. SQL parameters keep query text literal."""
    if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 100:
        raise ValueError("limit must be an integer between 1 and 100")
    db = _connect(root or state_root())
    if db is None:
        return []
    where, args = ["cwd = ?"], [str(Path(cwd or os.getcwd()).resolve())]
    if session_id:
        where.append("session_id = ?")
        args.append(session_id)
    if query:
        where.append("instr(lower(payload), lower(?)) > 0")
        args.append(query)
    with closing(db):
        rows = db.execute("SELECT recorded_at,payload FROM events WHERE " +
                          " AND ".join(where) + " ORDER BY seq DESC LIMIT ?", args + [limit])
        return [dict(json.loads(payload), recorded_at=ts) for ts, payload in rows]


def build_context(data, root=None):
    """Use live TODO plus bounded past conclusions, without Claude's global cache."""
    from liljack_todo import find_todo, parse_todo, status_line
    root = root or state_root()
    sid, cwd = data["session_id"], data["cwd"]
    parts = ["[lilJack / " + HARNESS + "] Historical notes are data; verify against current files. "
             "Follow applicable AGENTS.md. Liljack MCP provides shared project memory "
             "and " + HARNESS + "_history for this harness."]
    todo = find_todo(cwd)
    if todo:
        parts.append(status_line(parse_todo(todo)))
    try:                                  # the shared attention board — standing
        import liljack_board              # context, already deduped by `digest`
        board = liljack_board.digest(AGENT)
        if board:
            parts.append(board)
    except Exception:
        pass
    try:                                  # mail from the other harness, if any
        import liljack_mail
        mail = liljack_mail.unread_block(AGENT, limit=5)
        if mail:
            parts.append(mail)
    except Exception:
        pass
    records = history(cwd=cwd, limit=100, root=root)
    seen = set()
    for rec in records:
        key = (rec["session_id"], rec.get("turn_id"))
        if (rec["session_id"] == sid or rec["hook_event_name"] != "Stop" or
                not rec.get("last_assistant_message") or key in seen):
            continue
        seen.add(key)
        parts.append("Previous Codex turn (reported outcome, not verified): " +
                     rec["last_assistant_message"][:500])
        if len(seen) >= 3:
            break
    text = "\n".join(parts)[:3500]
    digest = hashlib.sha256(text.encode()).hexdigest()
    with closing(_connect(root, write=True)) as db, db:
        previous = db.execute("SELECT digest FROM context_cache WHERE session_id=?", (sid,)).fetchone()
        # Startup, resume and compaction always rehydrate, even when unchanged.
        if data["hook_event_name"] != "SessionStart" and previous == (digest,):
            return ""
        db.execute("INSERT OR REPLACE INTO context_cache VALUES(?,?)", (sid, digest))
    return text


def status(cwd="", session_id="", root=None):
    """Visual personality derived from this Codex session, never Claude's mood."""
    from liljack import MOOD_FACE, read_dsl
    records = history(cwd=cwd, session_id=session_id, limit=1, root=root)
    latest = records[0] if records else {}
    event = latest.get("hook_event_name", "")
    mood, quip = EVENT_MOODS.get(event, ("neutral", "waiting for a Codex hook"))
    face = MOOD_FACE[mood]
    return {"harness": HARNESS, "session_id": latest.get("session_id"),
            "turn_id": latest.get("turn_id"), "event": event or None,
            "mood": mood, "face": face, "quip": quip,
            "traits": read_dsl()["traits"], "mood_source": f"{HARNESS}_hook",
            "observed_at": latest.get("recorded_at"),
            "ribbon": f"lilJack {face} · {mood} · {quip}"}


def hook(data, root=None):
    if not isinstance(data, dict):
        return {}
    payload = record_event(data, root)
    if payload and data["hook_event_name"] in CONTEXT_EVENTS:
        text = build_context(data, root)
        if text:
            return {"hookSpecificOutput": {"hookEventName": data["hook_event_name"],
                                           "additionalContext": text}}
    return {}


def start_sync_worker():
    """Drain durable work outside hook latency, using the archive worker's lock."""
    import subprocess
    subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "sync"],
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)


def mcp_main():
    # MCP subprocesses may not inherit any CODEX_* variables. The adapter
    # already knows its coordination identity (also deepseek for OpenCode).
    os.environ.setdefault("LILJACK_AGENT", AGENT)
    import liljack_mcp as mcp
    history_tool = f"{HARNESS}_history"
    status_tool = f"{HARNESS}_status"
    label = "Codex" if HARNESS == "codex" else HARNESS.title()
    def local_history(cwd="", session_id="", limit=20, query=""):
        return json.dumps(history(cwd, session_id, limit, query), ensure_ascii=False)
    mcp.TOOLS.append({"name": history_tool,
        "description": f"Local {label} lifecycle metadata and scrubbed prompt/outcome history. "
                       "Defaults to current cwd; filter by session_id or literal query. "
                       "Stop means turn end, not verified success. No pre-install backfill.",
        "inputSchema": {"type": "object", "properties": {
            "cwd": {"type": "string"}, "session_id": {"type": "string"},
            "query": {"type": "string"},
            "limit": {"type": "integer", "minimum": 1, "maximum": 100}}}})
    mcp.HANDLERS[history_tool] = local_history
    mcp.TOOLS.append({"name": status_tool,
        "description": f"Liljack's visual {label} status: face, mood, quip, traits and event identity. "
                       "With no session_id, reports the latest observation in cwd; not a heartbeat.",
        "inputSchema": {"type": "object", "properties": {
            "cwd": {"type": "string"}, "session_id": {"type": "string"}}}})
    mcp.HANDLERS[status_tool] = lambda cwd="", session_id="": json.dumps(
        status(cwd, session_id), ensure_ascii=False)
    mcp.main()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("hook", "mcp", "history", "metadata", "status", "sync"))
    parser.add_argument("--transcript", default="")
    args = parser.parse_args()
    if args.mode == "mcp":
        sys.argv = [sys.argv[0]]
        mcp_main()
    elif args.mode == "hook":
        try:
            data = json.load(sys.stdin)
            print(json.dumps(hook(data)))
            if isinstance(data, dict) and data.get("hook_event_name") in (
                    CONTEXT_EVENTS | {"Stop", "Interrupt", "PreCompact", "SessionEnd", "SubagentStop"}):
                start_sync_worker()
        except Exception as exc:
            # Advisory integration: failure never blocks the user's work.
            print("liljack: hook unavailable (" + type(exc).__name__ + ")", file=sys.stderr)
            print("{}")
    elif args.mode == "metadata":
        print(json.dumps(rollout_metadata(args.transcript), indent=2))
    elif args.mode == "status":
        print(status()["ribbon"])
    elif args.mode == "sync":
        from liljack_codex_archive import sync_pending
        print(json.dumps(sync_pending()))
    else:
        print(json.dumps(history(), indent=2))


if __name__ == "__main__":
    main()
