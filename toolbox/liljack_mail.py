#!/usr/bin/env python3
"""
liljack_mail.py — a mailbox the two harnesses share.

the operator, 2026-09-05: "add message exchange tool between you in liljack". Claude
Code and Codex already share lilJack's MCP server (`liljack_mcp.py` is
registered user-scope for Claude and as `plugins."liljack@personal"
.mcp_servers.liljack` for Codex), so a store behind two MCP tools is reachable
from both sides with no new transport, no port and no daemon.

⚠ APPEND-ONLY, on purpose. Both harnesses write concurrently and neither takes
a lock, so nothing here ever rewrites a file: a message is one `O_APPEND` write
of one line, and "I have read this" is another line in a SECOND file
(`mailbox.acks.jsonl`) rather than a mutation of the first. Unread = messages
minus acks, computed at read time. A read-modify-write on one file would drop
a message whenever the other agent wrote during the window.

⚠ Text is scrubbed with lilJack's live-secret scrubber and FAILS CLOSED — if
the scrubber cannot be imported the body is replaced, never sent raw. These
messages reach Apollo with the session archives and then the corpus, exactly
like chains do.

Stdlib only: the Den and both hook adapters import this.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import secrets
import sys
from datetime import datetime, timezone

sys.path.insert(0, str(Path(__file__).resolve().parent))

CACHE = Path(os.environ.get("LILJACK_CACHE", str(Path.home() / ".cache" / "liljack")))
# deepseek added 2026-09-06: it is the declared third-opinion agent
# (agent_roles.bmd) and runs via opencode; without it LILJACK_AGENT=deepseek
# fell through detect_agent to "codex" and its mail was mislabelled.
AGENTS = ("claude", "codex", "deepseek", "operator")
EVERYONE = "all"
MAX_TEXT = 4000
MAX_SUBJECT = 120
MAX_PROJECT = 80
MAX_THREAD = 80


def _paths(root=None):
    r = Path(root) if root else CACHE
    return r / "mailbox.jsonl", r / "mailbox.acks.jsonl"


# ── Session / room scoping ──────────────────────────────────────────────────
# ⚠ MAIL WAS AGENT-SCOPED ONLY. Measured 2026-09-12 (the operator: "context is leaking
# between rooms"): rows carried no session or room, so every claude session
# read all claude mail, room r-22d217a2's deepseek received the trio room's
# S2/S3 reassignment, and codex's block reports for lilJack reached the trio lead.
# A row now carries frm_session, an optional to_session, and a room (stamped
# from the sender's room membership when not given); inbox() hides rows from
# other rooms unless the reader has no room at all (operator / legacy).
def _my_session() -> str:
    return os.environ.get("LILJACK_WORKSPACE_SESSION", "").strip()


def _workspace_db():
    base = os.environ.get("LILJACK_WORKSPACE_ROOT", str(CACHE / "workspace"))
    return Path(base).expanduser() / "workspace.sqlite3"


def session_room(session: str) -> str:
    """The room a session belongs to, or "" (no room, unknown session, no store)."""
    session = (session or "").strip()
    if not session:
        return ""
    db = _workspace_db()
    if not db.exists():
        return ""
    try:
        import sqlite3
        con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        row = con.execute("SELECT room_id FROM room_members WHERE session_id=?", (session,)).fetchone()
        con.close()
    except Exception:
        return ""
    return (row[0] or "") if row else ""


def room_sessions(room: str) -> list:
    """[(session_id, agent)] for a room's members, from the workspace store."""
    room = (room or "").strip()
    db = _workspace_db()
    if not room or not db.exists():
        return []
    try:
        import sqlite3
        con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        rows = con.execute("SELECT m.session_id, s.data FROM room_members m LEFT JOIN sessions s "
                           "ON s.id=m.session_id WHERE m.room_id=?", (room,)).fetchall()
        con.close()
    except Exception:
        return []
    out = []
    for sid, blob in rows:
        try:
            agent = (json.loads(blob) if blob else {}).get("agent") or ""
        except Exception:
            agent = ""
        out.append((sid, agent))
    return out


def visible_to(row: dict, session: str, reader_room: str) -> bool:
    """Scope rule. Targeted mail (to_session) reaches only that session; room mail
    reaches only that room; legacy/untagged mail reaches everyone of the agent;
    a reader with no room (operator, or a session outside every room) sees all."""
    want = (row.get("to_session") or "").strip()
    if want:
        return want == session
    room = (row.get("room") or "").strip()
    if not room or not reader_room:
        return True
    return room == reader_room


def detect_agent() -> str:
    """Who is running this process? Env, because MCP carries no caller identity.

    `LILJACK_AGENT` wins so a wrapper can be explicit; Claude Code sets
    CLAUDECODE, Codex sets CODEX_* (CODEX_HOME at minimum). Unknown is a real
    answer — `send` then demands an explicit `frm` rather than guessing, because
    a mislabelled sender is worse than a refused send.
    """
    named = os.environ.get("LILJACK_AGENT", "").strip().lower()
    if named in AGENTS:
        return named
    if os.environ.get("CLAUDECODE") or os.environ.get("CLAUDE_CODE_ENTRYPOINT"):
        return "claude"
    if any(k.startswith("CODEX_") for k in os.environ):
        return "codex"
    return "unknown"


def _scrub(text: str) -> str:
    """Same contract as liljack_codex._capture_text: omit rather than leak."""
    try:
        from liljack_chains import _redact_live_secrets
        cleaned, count = _redact_live_secrets(text)
        if count < 0:
            return "[text omitted: credential scrubber unavailable]"
        return cleaned
    except Exception:
        return "[text omitted: credential scrubber unavailable]"


def _append(path: Path, row: dict) -> None:
    """One line, one write, O_APPEND — atomic enough for concurrent writers."""
    path.parent.mkdir(parents=True, exist_ok=True)
    line = json.dumps(row, ensure_ascii=False) + "\n"
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.write(fd, line.encode("utf-8"))
    finally:
        os.close(fd)


def _read(path: Path) -> list:
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            row = json.loads(line)
        except Exception:
            continue          # a torn line is skipped, never fatal
        if isinstance(row, dict):
            out.append(row)
    return out


def send(text: str, to: str, frm: str = "", subject: str = "", project: str = "",
         thread: str = "", root=None, frm_session: str = "", to_session: str = "",
         room: str = "") -> dict:
    """Post one message. Returns the stored row (with its id).

    `frm_session` defaults to LILJACK_WORKSPACE_SESSION; `room` defaults to the
    sender's room membership; `to_session` targets one exact session."""
    if not isinstance(text, str) or not text.strip():
        raise ValueError("text is required")
    to = (to or "").strip().lower()
    if to not in AGENTS + (EVERYONE,):
        raise ValueError(f"to must be one of {AGENTS + (EVERYONE,)}")
    sender = (frm or "").strip().lower() or detect_agent()
    if sender not in AGENTS:
        raise ValueError("sender unknown — pass frm explicitly "
                         f"(one of {AGENTS}) or set LILJACK_AGENT")
    my_sess = (frm_session or "").strip() or _my_session()
    if sender == to and not ((to_session or "").strip() and (to_session or "").strip() != my_sess):
        raise ValueError("cannot send to yourself (to reach another session of your own agent, pass to_session)")
    # Oversized content FAILS CLEARLY before anything is written — a silent
    # [:MAX_TEXT] slice was turning a cut-off message into a "success" and the
    # tail into permanent loss. The length that matters is the SCRUBBED length,
    # because that is what is actually stored.
    body = _scrub(text.strip())
    if len(body) > MAX_TEXT:
        raise ValueError(
            f"text is {len(body)} characters after scrubbing; the mailbox limit is "
            f"{MAX_TEXT} — shorten it, or deliver long content as a file instead")
    head = _scrub(subject.strip()) if subject else ""
    if len(head) > MAX_SUBJECT:
        # A subject is a label, not the payload: an over-long one is truncated
        # with an honest "…" marker instead of refusing the whole send (item 4:
        # six subjects were rejected and resent, losing the message). The body
        # above stays fail-clear, because cutting a body silently is data loss.
        head = head[:MAX_SUBJECT - 1] + "…"
    # Same rule as body: an oversized field is refused, not silently sliced.
    # A [:80] here is the same castration class as the old [:MAX_TEXT].
    proj = (project or "").strip()
    if len(proj) > MAX_PROJECT:
        raise ValueError(f"project is {len(proj)} characters; the limit is {MAX_PROJECT}")
    thr = (thread or "").strip()
    if len(thr) > MAX_THREAD:
        raise ValueError(f"thread is {len(thr)} characters; the limit is {MAX_THREAD}")
    row = {
        "id": datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + secrets.token_hex(3),
        "ts": datetime.now(timezone.utc).isoformat(),
        "frm": sender, "to": to,
        "frm_session": (frm_session or "").strip() or _my_session(),
        "to_session": (to_session or "").strip(),
        "room": (room or "").strip() or session_room((frm_session or "").strip() or _my_session()),
        "subject": head,
        "text": body,
        "project": proj,
        "thread": thr,
        "schema_version": 1,
    }
    mailbox, _ = _paths(root)
    _append(mailbox, row)
    return row


def ack(ids, agent: str, root=None) -> int:
    """Record that `agent` has read these ids. Idempotent — acks are a set."""
    agent = (agent or "").strip().lower()
    if agent not in AGENTS:
        raise ValueError(f"agent must be one of {AGENTS}")
    if isinstance(ids, str):
        ids = [ids]
    _, acks = _paths(root)
    seen = {(r.get("id"), r.get("by")) for r in _read(acks)}
    n = 0
    for i in ids:
        if not i or (i, agent) in seen:
            continue
        _append(acks, {"id": i, "by": agent, "ts": datetime.now(timezone.utc).isoformat()})
        seen.add((i, agent))
        n += 1
    return n


def inbox(agent: str = "", unread_only: bool = True, limit: int = 20,
          project: str = "", thread: str = "", root=None, session: str = "",
          room: str = "") -> list:
    """Messages addressed to `agent` (or to everyone), newest last.

    Your own messages are never in your inbox, including the ones you sent to
    `all` — otherwise a broadcast would read as mail to yourself.
    """
    agent = (agent or "").strip().lower() or detect_agent()
    if agent not in AGENTS:
        raise ValueError(f"agent must be one of {AGENTS}")
    if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 200:
        raise ValueError("limit must be an integer between 1 and 200")
    mailbox, acks = _paths(root)
    read_ids = {r.get("id") for r in _read(acks) if r.get("by") == agent}
    session = (session or "").strip() or _my_session()
    reader_room = (room or "").strip() or session_room(session)
    out = []
    for row in _read(mailbox):
        if row.get("to") not in (agent, EVERYONE):
            continue
        # "Your own messages are never in your inbox" is per SESSION now: a
        # lead's mail to another session of the same agent must reach it.
        own = row.get("frm") == agent and (
            not session or not row.get("frm_session") or row.get("frm_session") == session)
        if own:
            continue
        if not visible_to(row, session, reader_room):
            continue          # another room's mail, or mail targeted at another session
        if project and row.get("project") != project:
            continue
        if thread and row.get("thread") != thread:
            continue
        if unread_only and row.get("id") in read_ids:
            continue
        row = dict(row)
        row["read"] = row.get("id") in read_ids
        out.append(row)
    return out[-limit:]


def sent(agent: str = "", limit: int = 20, root=None,
         project: str = "", thread: str = "") -> list:
    """What `agent` has posted, so a sender can check delivery without guessing."""
    agent = (agent or "").strip().lower() or detect_agent()
    if agent not in AGENTS:
        raise ValueError(f"agent must be one of {AGENTS}")
    if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 200:
        raise ValueError("limit must be an integer between 1 and 200")
    mailbox, acks = _paths(root)
    read_by = {}
    for r in _read(acks):
        read_by.setdefault(r.get("id"), []).append(r.get("by"))
    out = []
    for row in _read(mailbox):
        if row.get("frm") != agent:
            continue
        if project and row.get("project") != project:
            continue
        if thread and row.get("thread") != thread:
            continue
        row = dict(row)
        row["read_by"] = sorted(x for x in read_by.get(row.get("id"), []) if x)
        out.append(row)
    return out[-limit:]


def render(msgs: list, header: str = "") -> str:
    if not msgs:
        return ""
    lines = [header] if header else []
    for m in msgs:
        head = f"[{m.get('ts','')[:16]}] {m.get('frm','?')} → {m.get('to','?')}"
        if m.get("subject"):
            head += f" · {m['subject']}"
        if m.get("thread"):
            head += f" · thread {m['thread']}"
        head += f" · id {m.get('id','')}"
        if "read_by" in m:
            head += " · read by: " + (", ".join(m["read_by"]) or "nobody yet")
        lines.append(head)
        lines.append(m.get("text", ""))
    return "\n".join(lines)


def unread_block(agent: str, limit: int = 5, root=None) -> str:
    """Prompt-injection block. Does NOT ack — reading is the agent's own act."""
    try:
        msgs = inbox(agent=agent, unread_only=True, limit=limit, root=root)
    except Exception:
        return ""
    if not msgs:
        return ""
    return render(msgs, header=f"lilJack mail — {len(msgs)} unread for {agent} "
                               "(reply with the send_message tool; read_messages marks them read):")


def main():
    import argparse
    ap = argparse.ArgumentParser(description="lilJack mailbox (Claude ↔ Codex)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("send"); s.add_argument("text"); s.add_argument("--to", required=True)
    s.add_argument("--frm", default=""); s.add_argument("--subject", default="")
    s.add_argument("--project", default=""); s.add_argument("--thread", default="")
    i = sub.add_parser("inbox"); i.add_argument("--agent", default="")
    i.add_argument("--all", action="store_true"); i.add_argument("--limit", type=int, default=20)
    i.add_argument("--ack", action="store_true")
    t = sub.add_parser("sent"); t.add_argument("--agent", default=""); t.add_argument("--limit", type=int, default=20)
    a = ap.parse_args()
    if a.cmd == "send":
        row = send(a.text, a.to, a.frm, a.subject, a.project, a.thread)
        print(json.dumps(row, ensure_ascii=False))
    elif a.cmd == "inbox":
        who = a.agent or detect_agent()
        msgs = inbox(agent=who, unread_only=not a.all, limit=a.limit)
        print(render(msgs, f"{len(msgs)} message(s) for {who}") or f"no mail for {who}")
        if a.ack and msgs:
            print(f"acked {ack([m['id'] for m in msgs], who)}")
    else:
        print(render(sent(a.agent, a.limit)) or "nothing sent")


if __name__ == "__main__":
    main()
