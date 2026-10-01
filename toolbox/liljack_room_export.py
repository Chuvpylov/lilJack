#!/usr/bin/env python3
"""
liljack_room_export.py — scrubbed room-log export into mcp-projects/<slug>/rooms/.

Authorised by lead claude (2026-09-09), decision D1–D3:
  D1  repo layout = mcp-projects/<project>/rooms/   (per-project derived artifact,
      same repo liljack_projects already writes to — NOT a new liljack-rooms repo)
  D2  cadence      = called by the existing 5-minute sync worker; NO new daemon
  D3  store        = read ~/.cache/liljack/workspace in place; syncing removes the
      single-point-of-failure, so the store does not move (yet).

What it does (standalone, read-only on the store, writes only its own mirror):
  1. Read the workspace room record: messages + frozen audience, rooms, members,
     team roles, and the event cursor.
  2. Scrub every message text through the live-secret scrubber, fail-closed
     (omission marker, never raw text).
  3. Write <MIRROR>/<slug>/rooms/rooms.jsonl + rooms.meta.json, idempotent via a
     sha1 ledger.
  4. Optional Apollo push (repo mcp-projects) using session_archive's pusher,
     exactly like liljack_projects does. --dry-run / --no-push never push.

This module does NOT wire itself into sync_pending (that call is the lead's/
codex's integration); it is importable as `liljack_room_export.build(...)`.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from liljack_workspace import Workspace, project_key  # noqa: E402
from session_archive import (ARCHIVE_ROOT, _apollo_push_file, _apollo_url,  # noqa: E402
                             _project_slug)

MIRROR = ARCHIVE_ROOT / "mcp-projects"
REPO = "mcp-projects"
REDACTION_MARK = "[REDACTED]"
OMISSION_MARK = "[text omitted: credential scrubber unavailable]"


def scrub(text: str) -> tuple[str, int]:
    """(scrubbed, hits). Fail closed: a broken scrubber omits, never leaks."""
    try:
        from liljack_chains import _redact_live_secrets
        cleaned, count = _redact_live_secrets(text)
        if count < 0:
            return OMISSION_MARK, -1
        return cleaned, count
    except Exception:
        return OMISSION_MARK, -1


def read_room_record(root=None, project=None) -> dict:
    """Extract the exportable room record for one project, read-only."""
    store = Workspace(root)
    try:
        project = project_key(project or os.getcwd())
        slug = _project_slug(project) or "unknown"
        rows = store.db.execute(
            "SELECT * FROM messages WHERE project=? ORDER BY seq", (project,)).fetchall()
        recipients = {}
        for r in store.db.execute(
                "SELECT message_id, recipient FROM message_recipients "
                "WHERE message_id IN (SELECT id FROM messages WHERE project=?)",
                (project,)):
            recipients.setdefault(r["message_id"], []).append(r["recipient"])
        sessions = {}
        for r in store.db.execute(
                "SELECT id, data FROM sessions WHERE project=?", (project,)):
            try:
                d = json.loads(r["data"])
            except ValueError:
                d = {}
            sessions[r["id"]] = {"agent": d.get("agent") or "", "harness": d.get("harness") or ""}
        rooms = [dict(r) for r in store.db.execute(
            "SELECT * FROM rooms WHERE project=? ORDER BY rowid", (project,))]
        members = [dict(r) for r in store.db.execute(
            "SELECT rm.session_id, rm.room_id FROM room_members rm "
            "JOIN rooms ro ON ro.id=rm.room_id WHERE ro.project=?", (project,))]
        team = [dict(r) for r in store.db.execute(
            "SELECT t.session_id, t.role, t.parent_id FROM team t "
            "JOIN sessions s ON s.id=t.session_id WHERE s.project=?", (project,))]
        cursor = store.db.execute(
            "SELECT COALESCE(MAX(seq),0) FROM events WHERE project=?", (project,)).fetchone()[0]
    finally:
        store.close()

    messages = []
    total_hits = 0
    omissions = 0
    for m in rows:
        audience = sorted(recipients.get(m["id"], []))
        text, hits = scrub(m["text"])
        total_hits += max(hits, 0)
        if hits < 0:
            omissions += 1
        who = sessions.get(m["sender"], {})
        messages.append({
            "id": m["id"], "seq": m["seq"], "sender": m["sender"],
            "destination": m["destination"], "audience": audience,
            "reply_to": m["reply_to"], "language": m["language"],
            "created_at": m["created_at"], "text": text,
            "agent": who.get("agent", ""), "harness": who.get("harness", ""),
        })
    return {"slug": slug, "project": project, "cursor": cursor,
            "rooms": rooms, "members": members, "team": team,
            "messages": messages, "redactions": total_hits, "omissions": omissions,
            "count": len(messages)}


def _meta(record: dict) -> dict:
    return {"slug": record["slug"], "project": record["project"],
            "cursor": record["cursor"], "message_count": record["count"],
            "redactions": record["redactions"], "omissions": record["omissions"],
            "rooms": record["rooms"], "members": record["members"],
            "team": record["team"]}


def build_mirror(record: dict, mirror=None) -> dict:
    """Write rooms.jsonl + rooms.meta.json under <mirror>/<slug>/rooms/.

    Idempotent: a file is rewritten only when its content changed. Returns
    {"written": [relpaths], "skipped": [relpaths], "root": str(root)}.
    """
    mirror = Path(mirror) if mirror else MIRROR
    root = mirror / record["slug"] / "rooms"
    root.mkdir(parents=True, exist_ok=True)
    blob = ("\n".join(json.dumps(m, ensure_ascii=False) for m in record["messages"]) + "\n")
    blob = blob if record["messages"] else ""
    meta_blob = json.dumps(_meta(record), ensure_ascii=False, indent=1).encode()
    written, skipped = [], []

    def _write(rel: str, data: bytes):
        p = root / rel
        if p.exists() and p.read_bytes() == data:
            skipped.append(rel)
            return
        p.write_bytes(data)
        written.append(rel)

    _write("rooms.jsonl", blob.encode())
    _write("rooms.meta.json", meta_blob)
    return {"written": written, "skipped": skipped, "root": str(root)}


def export(root=None, project=None, push=False, mirror=None):
    """Build the mirror and (optionally) push to Apollo repo mcp-projects."""
    record = read_room_record(root, project)
    built = build_mirror(record, mirror)
    pushed = []
    if push:
        url = _apollo_url()
        if url:
            root_dir = Path(built["root"])
            for rel in ("rooms.jsonl", "rooms.meta.json"):
                p = root_dir / rel
                if p.exists() and not _apollo_push_file(
                        url, REPO, f"{record['slug']}/rooms/{rel}", p.read_bytes()):
                    built.setdefault("push_failed", []).append(rel)
                    continue
                if p.exists():
                    pushed.append(rel)
        else:
            built["push_unavailable"] = True
    built["pushed"] = pushed
    built.update(slug=record["slug"], messages=record["count"],
                 redactions=record["redactions"], omissions=record["omissions"],
                 cursor=record["cursor"])
    return built


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root")
    ap.add_argument("--project", default=os.getcwd())
    ap.add_argument("--push", action="store_true", help="push to Apollo (default: local mirror only)")
    ap.add_argument("--dry-run", action="store_true", help="read+scrub only; write nothing, push nothing")
    args = ap.parse_args(argv)
    if args.dry_run:
        record = read_room_record(args.root, args.project)
        print(json.dumps({"slug": record["slug"], "messages": record["count"],
                          "redactions": record["redactions"],
                          "omissions": record["omissions"], "cursor": record["cursor"]},
                         ensure_ascii=False, indent=1))
        return 0
    print(json.dumps(export(args.root, args.project, push=args.push), ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
