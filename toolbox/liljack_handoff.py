#!/usr/bin/env python3
"""Handoff digest — compact a session's OWN record before a room move.

the operator, 2026-09-12: "structure room migration as a process — compacting before
moving, all the garbage left behind; the full session is always accessible
through Apollo's session store." The garbage is the backlog: the lead received
~20 replayed notices from 02:22-03:10, every one already handled.

A digest is a DIGEST, never bodies (the 2026-09-07 mail-digest lesson: a full
render is 75% of the payload and moves the hash every turn). One line per item,
size-capped, and it points at the full transcript in Apollo.

⚠ COMPACTION IS DERIVED; IT REPLACES NOTHING. The archive, index, chains and
Apollo stores are read-only here. The digest is a new file written BESIDE the
index (`<archive>/handoff/<session>.json`), and the test asserts the archive and
index are byte-identical before and after a compact+move.
"""
from __future__ import annotations

import json
import os
import re
from pathlib import Path

import liljack_tasks as tasks
from liljack_workspace import project_key, utcnow

DIGEST_MAX_BYTES = 4000
HANDOFF_DIR = "handoff"


def default_archive_root() -> Path:
    return Path(os.environ.get("LILJACK_ARCHIVE_ROOT",
                               str(Path.home() / ".claude" / "session_archive")))


def _one_line(text, n: int) -> str:
    return re.sub(r"\s+", " ", str(text or "")).strip()[:n]


def _index_entry(archive_root: Path, keys) -> dict | None:
    """The archive index record for a session, by any of `keys` (native id first)."""
    index = archive_root / "index.jsonl"
    if not index.exists():
        return None
    wanted = {str(k) for k in keys if k}
    for line in index.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if rec.get("session_id") in wanted:
            return rec
    return None


_PATH_RE = re.compile(r"(?:toolbox|tests|docs|data|arena|hui|the trainer)/[\w./-]+\.\w{1,6}")
_EXT_RE = re.compile(r"\b[\w.-]+\.(?:py|c|h|md|json|jsonl|yaml|yml|sh|txt)\b")


def _files_from(messages) -> list:
    """Files a session ANNOUNCED ownership of — repo-relative paths in its posts.

    Heuristic and labelled as such: a path under a known tree (toolbox/, tests/,
    docs/, ...) that appears in the session's own messages. Bare basenames like
    `main.c` are NOT counted — they are too weak to be an ownership claim.
    Deduped, order kept, capped.
    """
    found, seen = [], set()
    for m in messages:
        for tok in _PATH_RE.findall(m.get("text") or ""):
            tok = tok.strip(".,;:)`'\"")
            if tok not in seen:
                seen.add(tok)
                found.append(tok)
    return found[:20]


def _spec_pointer(purpose: str) -> str:
    m = re.search(r"[\w./-]+\.md", purpose or "")
    return m.group(0) if m else ""


def build_digest(store, project, session_id, *, task_path=None, archive_root=None) -> dict:
    """The structured handoff digest for one session, from its own record."""
    project = project_key(project)
    row = store.db.execute("SELECT data FROM sessions WHERE id=?", (session_id,)).fetchone()
    if not row:
        raise ValueError(f"unknown session {session_id}")
    data = json.loads(row["data"])
    agent, native = data.get("agent"), data.get("native_id")

    here = store.db.execute("SELECT room_id FROM room_members WHERE session_id=?",
                            (session_id,)).fetchone()
    room_id = here["room_id"] if here else ""
    room = store._room(project, room_id) if room_id else {}

    held = [t for t in tasks.tasks(open_only=False, path=task_path)
            if (str(t.get("owner_session") or "") == session_id
                or (not t.get("owner_session")
                    and str(t.get("owner") or "").lower() == str(agent or "").lower()))]

    msgs = [store._message(r) for r in store.db.execute(
        "SELECT * FROM messages WHERE project=? AND sender=? ORDER BY seq DESC LIMIT 12",
        (project, session_id))]

    questions = []
    try:
        from .room_protocol import question_digest
        questions = [q for q in question_digest(store, project, room_id)
                     if str(q.get("asker") or "") == session_id]
    except Exception:
        questions = []

    root = Path(archive_root) if archive_root else default_archive_root()
    rec = _index_entry(root, [native, session_id])
    apollo = f"apollo://liljack-sessions/{rec['archive_path']}" if rec and rec.get("archive_path") else ""

    return {
        "session": session_id, "agent": agent, "native_id": native,
        "room": room_id, "room_name": room.get("name", ""),
        "spec": _spec_pointer(room.get("purpose", "")),
        "purpose": _one_line(room.get("purpose", ""), 300),
        # OPEN work is what the session still HOLDS; closed work is history and
        # is summarised, not listed — the compact is the point.
        "tasks": [{"id": t["id"], "state": t["state"], "title": t["title"],
                   "room": str(t.get("room") or "")}
                  for t in held if t["state"] not in ("done", "abandoned")],
        "closed_count": sum(1 for t in held if t["state"] in ("done", "abandoned")),
        "verdicts": [{"seq": m["seq"], "text": _one_line(m["text"], 160)} for m in msgs],
        "files": _files_from(msgs),
        "questions": [{"id": q["id"], "task": q.get("task", ""),
                       "text": _one_line(q.get("question", ""), 120)} for q in questions],
        "archive_path": (rec or {}).get("archive_path", ""),
        "apollo": apollo,
        "built_at": utcnow(),
    }


def render_digest(d: dict, max_bytes: int = DIGEST_MAX_BYTES, room_id: str | None = None) -> str:
    """One-line-per-item text, hard-capped, pointing at the full transcript.

    ⚠ room_id, WHEN GIVEN, IS THE ROOM THIS TEXT IS POSTED INTO. A task filed to
    a DIFFERENT room must not be named in this room's intro or K4 post (the
    cross-room leak the operator hit at 01:10). Unfiled tasks stay eligible. With
    room_id=None (the on-disk digest) the full list is kept — the file is the
    session's own record, not a room's.
    """
    tasks_shown, filed_away = [], 0
    for t in d.get("tasks", []):
        filed = str(t.get("room") or "").strip()
        if room_id is not None and filed and filed != room_id:
            filed_away += 1
            continue
        tasks_shown.append(t)
    lines = [f"HANDOFF DIGEST — {d.get('agent')} {d.get('session')}",
             f"moved from room: {d.get('room_name') or d.get('room') or '(none)'}",
             f"full transcript: {d.get('apollo') or 'apollo://liljack-sessions (not indexed yet)'}"]
    if d.get("spec"):
        lines.append(f"spec: {d['spec']}")
    lines.append("tasks you hold:")
    lines += [f"- [{t['state']}] {t['id']}: {_one_line(t['title'], 100)}" for t in tasks_shown] or ["- (none)"]
    if filed_away:
        lines.append(f"({filed_away} task(s) filed to another room — not shown here)")
    if d.get("closed_count"):
        lines.append(f"(+{d['closed_count']} closed task(s) — history, in the registry)")
    if d.get("files"):
        lines.append("files you announced owning:")
        lines += [f"- {f}" for f in d["files"]]
    if d.get("questions"):
        lines.append("open questions you asked:")
        lines += [f"- {q['id']} ({q.get('task') or '?'}): {q['text']}" for q in d["questions"]]
    if d.get("verdicts"):
        lines.append("your last verdicts/decisions:")
        lines += [f"- {v['text']}" for v in d["verdicts"]]
    text = "\n".join(lines)
    if len(text) > max_bytes:
        suffix = "\n… digest truncated; the full transcript is in Apollo."
        text = text[:max(0, max_bytes - len(suffix))] + suffix
    return text


def write_digest(session_id: str, digest: dict, *, archive_root=None) -> dict:
    """Write the digest BESIDE the index. Never touches the index or the archive."""
    root = Path(archive_root) if archive_root else default_archive_root()
    out_dir = root / HANDOFF_DIR
    out_dir.mkdir(parents=True, exist_ok=True)
    jpath = out_dir / f"{session_id}.json"
    tpath = out_dir / f"{session_id}.txt"
    jpath.write_text(json.dumps(digest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    tpath.write_text(render_digest(digest) + "\n", encoding="utf-8")
    return {"json": str(jpath), "text": str(tpath), "bytes": len(render_digest(digest))}


def load_digest(session_id: str, *, archive_root=None) -> dict | None:
    root = Path(archive_root) if archive_root else default_archive_root()
    jpath = root / HANDOFF_DIR / f"{session_id}.json"
    if not jpath.exists():
        return None
    try:
        return json.loads(jpath.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def compact(store, project, session_id, *, task_path=None, archive_root=None) -> dict:
    """Build + write the digest. Returns the structured digest plus its paths."""
    digest = build_digest(store, project, session_id, task_path=task_path,
                          archive_root=archive_root)
    paths = write_digest(session_id, digest, archive_root=archive_root)
    return {**digest, "written": paths, "text": render_digest(digest)}
