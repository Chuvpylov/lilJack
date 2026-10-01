#!/usr/bin/env python3
"""
liljack_opencode_transcript.py — export an opencode session as a Codex-format
JSONL transcript, so the shared archive queue can ingest opencode sessions the
same way it ingests codex and claude ones.

WHY THIS EXISTS: opencode stores sessions in its own SQLite store
(`~/.local/share/opencode/opencode.db`), NOT as the codex-style JSONL that
`liljack_codex.rollout_metadata` and `liljack_codex_archive.sync_pending`
already read. The adapter therefore could never supply a `transcript_path`, so
`record_event` never queued an archive and opencode's history lived on this box
alone (the capture gap claude audited). This module exports the SQLite rows to
the JSONL shape the archive machinery already understands.

The JSONL is the SOURCE archive (raw, unscrubbed): sync_pending scrubs it later.
It is written under the opencode state root (`~/.opencode/liljack/transcripts/`),
alongside the lifecycle store. Read-only on opencode.db; writes only its own
mirror file, atomically.
"""
from __future__ import annotations

import json
import os
import sqlite3
from datetime import datetime, timezone
from pathlib import Path


def _opencode_db() -> Path:
    return Path(os.environ.get(
        "LILJACK_OPENCODE_DB",
        str(Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share")))
            / "opencode" / "opencode.db")))


def _out_dir() -> Path:
    try:
        from liljack_codex import state_root
        return Path(state_root()) / "transcripts"
    except Exception:
        return Path(os.environ.get("LILJACK_OPENCODE_STATE",
                                   str(Path.home() / ".opencode" / "liljack"))) / "transcripts"


def _iso(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).isoformat()


def _text_content(parts: list[dict]) -> list[dict]:
    out = []
    for p in parts:
        t = p.get("type")
        if t == "text" and isinstance(p.get("text"), str) and p["text"].strip():
            out.append({"type": "input_text" if False else "output_text",
                        "text": p["text"]})
    return out


def export(session_id: str, db_path=None, out_dir=None) -> Path | None:
    """Export one opencode session to a Codex-shape JSONL; return its path.

    Returns None when the session does not exist or has nothing to archive —
    never raises, so a hook that fails to export still records its event.
    """
    if not session_id:
        return None
    db = Path(db_path) if db_path else _opencode_db()
    if not db.exists():
        return None
    try:
        conn = sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=3)
    except sqlite3.Error:
        return None
    conn.row_factory = sqlite3.Row
    try:
        sess = conn.execute(
            "SELECT id, directory, title, model, time_created FROM session WHERE id=?",
            (session_id,)).fetchone()
        if not sess:
            return None
        messages = conn.execute(
            "SELECT id, json_extract(data,'$.role') AS role, time_created "
            "FROM message WHERE session_id=? ORDER BY time_created, id",
            (session_id,)).fetchall()
        parts = conn.execute(
            "SELECT message_id, data, time_created FROM part WHERE session_id=? "
            "ORDER BY time_created, id", (session_id,)).fetchall()
    finally:
        conn.close()

    by_msg: dict[str, list[dict]] = {}
    for p in parts:
        try:
            d = json.loads(p["data"])
        except (TypeError, ValueError):
            continue
        by_msg.setdefault(p["message_id"], []).append(d)

    lines: list[str] = []
    meta = {"id": session_id, "session_id": session_id,
            "cwd": sess["directory"], "timestamp": _iso(sess["time_created"]),
            "cli_version": "opencode", "model_provider": "deepseek",
            "history_mode": "opencode-sqlite", "source": "opencode"}
    lines.append(json.dumps({"type": "session_meta", "payload": meta}))

    for m in messages:
        role = m["role"] or "assistant"
        mparts = by_msg.get(m["id"], [])
        text_parts = [d for d in mparts if d.get("type") == "text" and d.get("text")]
        if role == "user" and text_parts:
            content = [{"type": "input_text", "text": t["text"]} for t in text_parts]
            lines.append(json.dumps({"type": "response_item", "payload": {
                "type": "message", "role": "user", "content": content}}))
        elif role == "assistant":
            if text_parts:
                content = [{"type": "output_text", "text": t["text"]} for t in text_parts]
                lines.append(json.dumps({"type": "response_item", "payload": {
                    "type": "message", "role": "assistant", "phase": "final_answer",
                    "content": content}}))
            for d in mparts:
                if d.get("type") != "tool":
                    continue
                st = d.get("state") or {}
                call_id = d.get("callID") or d.get("call_id") or ""
                name = d.get("tool") or d.get("name") or "unknown"
                inp = st.get("input") if isinstance(st.get("input"), str) else json.dumps(st.get("input") or {})
                lines.append(json.dumps({"type": "response_item", "payload": {
                    "type": "custom_tool_call", "name": name, "call_id": call_id,
                    "input": inp}}))
                out = st.get("output") or st.get("error") or ""
                lines.append(json.dumps({"type": "response_item", "payload": {
                    "type": "custom_tool_call_output", "call_id": call_id,
                    "output": out if isinstance(out, str) else json.dumps(out)}}))

    if len(lines) <= 1:
        return None  # nothing beyond the meta header — nothing to archive
    out = Path(out_dir) if out_dir else _out_dir()
    out.mkdir(parents=True, exist_ok=True, mode=0o700)
    safe = "".join(c if (c.isalnum() or c in "-_.") else "_" for c in session_id)[:120]
    dest = out / f"{safe}.jsonl"
    tmp = dest.with_suffix(".jsonl.tmp")
    tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.replace(tmp, dest)
    return dest


def backfill(db_path=None, dry_run=False):
    """Export every real opencode session and queue it for the shared archive.

    Reads the opencode `session` table (real sessions only — test fixtures live
    in the lifecycle events, never here), exports each non-empty one, and
    INSERT OR REPLACEs it into archive_queue so the next sync_pending archives
    and pushes it. Returns {queued, skipped, total}.
    """
    from contextlib import closing
    from datetime import datetime, timezone
    import liljack_codex as _codex

    db = Path(db_path) if db_path else _opencode_db()
    if not db.exists():
        return {"queued": 0, "skipped": [], "total": 0, "error": "opencode.db not found"}
    conn = sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=3)
    conn.row_factory = sqlite3.Row
    try:
        sessions = conn.execute(
            "SELECT id, directory FROM session ORDER BY time_created").fetchall()
    finally:
        conn.close()

    queued, skipped = 0, []
    if dry_run:
        for s in sessions:
            p = export(s["id"], db_path=db)
            if p:
                queued += 1
            else:
                skipped.append(s["id"])
        return {"queued": queued, "skipped": skipped, "total": len(sessions)}

    with closing(_codex._connect(_codex.state_root(), write=True)) as wdb, wdb:
        for s in sessions:
            p = export(s["id"], db_path=db)
            if not p:
                skipped.append(s["id"])
                continue
            revision = datetime.now(timezone.utc).isoformat()
            wdb.execute("INSERT OR REPLACE INTO archive_queue VALUES(?,?,?,?)",
                        (s["id"], s["directory"] or "", str(p), revision))
            queued += 1
    return {"queued": queued, "skipped": skipped, "total": len(sessions)}


if __name__ == "__main__":
    import argparse
    import json
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("session_id", nargs="?")
    ap.add_argument("--backfill", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    if args.backfill:
        print(json.dumps(backfill(dry_run=args.dry_run), ensure_ascii=False, indent=1))
    elif args.session_id:
        print(export(args.session_id) or "")
    else:
        ap.error("provide session_id or --backfill")
