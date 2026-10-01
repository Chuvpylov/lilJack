#!/usr/bin/env python3
"""
liljack_room_log.py — the Room Log: each room message as a verified BMD envelope.

Emits claude's verified envelope (App-review msg#224) into a per-project
`room-log.bmd`, co-located with the room export (`mcp-projects/<slug>/rooms/`).

Shape, one message = one pasem, prose = scrubbed text, footer = claims:
    [[pasem: m-<seq>]]
    <scrubbed message text>
    [[footer]]
    fact(room-msg, m-<seq>, <sender> | room=<dest> | at=<ts> | agent=<agent>)
    edge(replies-to, m-<seq>, m-<target-seq>)          # only when reply_to is set
    [[/footer]]
    [[/pasem]]
Document footer:
    fact(room-log, <slug> | messages=<n> | redactions=<n> | updated=<ts>)

SCOPE, honest: this writer emits the MECHANICAL claims — message identity and
reply topology — that can be derived read-only from the store. The SEMANTIC
claims (kind=, msg-claim, msg-file, neg, rule) require the author to classify
its own message and are NOT fabricated here; `build()` accepts an optional
`extra` mapping (message seq -> [assertions]) for exactly that, and otherwise
leaves them absent rather than inventing a kind it cannot know.

VERIFIED, not sketched: the rendered text round-trips through
the trainer/corpus/bmd.py — same assertion count, idempotent on re-render. Claims
live in [[footer]] only; the three BMD traps from msg#224 (footer-only claims,
rule attr re-emission, never diff bytes) are honoured by construction.

Reads the workspace store read-only (via liljack_room_export); writes only its
own mirror file, under the shared board lock, atomically.
"""
from __future__ import annotations

import os
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from liljack_board import bmd, lock, write_atomic, qattr_all, check_arg  # noqa: E402
from liljack_room_export import read_room_record, MIRROR  # noqa: E402

PRED_MSG = "room-msg"
PRED_LOG = "room-log"
REL_REPLIES = "replies-to"


def _now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ")


def _subject(seq) -> str:
    return f"m-{seq}"


def default_path(record: dict, mirror=None) -> Path:
    mirror = Path(mirror) if mirror else MIRROR
    return mirror / record["slug"] / "rooms" / "room-log.bmd"


def build(record: dict, extra=None) -> str:
    """Build the room-log BMD text for one project's record.

    `record` is the dict from liljack_room_export.read_room_record(). `extra`,
    when given, maps message seq -> list of bmd Assertion objects (the author's
    semantic claims: msg-claim / msg-file / neg / rule) to append after the
    mechanical claims.
    """
    B = bmd()
    extra = extra or {}
    doc = B.BmdDoc()
    doc.header = {
        "title": f"room log — {record['slug']}",
        "kind": "room-log",
        "project": record["project"],
        "version": "1",
        "updated": _now(),
    }
    id_to_seq = {m["id"]: m["seq"] for m in record["messages"]}
    doc.pasems = []
    for m in record["messages"]:
        sender = m["agent"] or m["sender"]
        attrs = {"room": m["destination"], "at": m["created_at"] or _now()}
        if m["agent"]:
            attrs["agent"] = m["agent"]
        claims = [B.fact(PRED_MSG, _subject(m["seq"]), check_arg(sender, "sender"),
                         **qattr_all(**attrs))]
        if m["reply_to"] and m["reply_to"] in id_to_seq:
            claims.append(B.edge(REL_REPLIES, _subject(m["seq"]),
                                 _subject(id_to_seq[m["reply_to"]])))
        claims += list(extra.get(m["seq"], []))
        doc.pasems.append(B.Pasem(name=_subject(m["seq"]), text=m["text"],
                                  assertions=claims))
    doc.assertions = [B.fact(PRED_LOG, check_arg(record["slug"], "slug"),
                             **qattr_all(messages=record["count"],
                                         redactions=record["redactions"],
                                         updated=_now()))]
    return doc.render()


def verify(text: str) -> dict:
    """Round-trip the rendered text through the trainer's parser.

    Returns {"ok": bool, "assertions": int, "idempotent": bool, "error": str}.
    This is the acceptance test: the parser must read back what we wrote.
    """
    B = bmd()
    try:
        doc = B.parse(text, strict=True)
    except Exception as exc:  # BmdError or anything — never raise into a hook
        return {"ok": False, "assertions": 0, "idempotent": False,
                "error": f"{type(exc).__name__}: {exc}"}
    n = len(doc.all_assertions())
    idem = (doc.render() == text)
    return {"ok": True, "assertions": n, "idempotent": idem, "error": ""}


def write(record: dict, extra=None, path=None, mirror=None) -> dict:
    """Build + verify + atomically write the room log. Never writes a file the
    parser would reject."""
    text = build(record, extra)
    check = verify(text)
    if not check["ok"] or not check["idempotent"]:
        return {"written": False, **check}
    p = Path(path) if path else default_path(record, mirror)
    with lock(p):
        write_atomic(p, text)
    return {"written": True, "path": str(p), **check}


def main(argv=None):
    import argparse
    import json
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root")
    ap.add_argument("--project", default=os.getcwd())
    ap.add_argument("--dry-run", action="store_true",
                    help="build + verify only; write nothing")
    ap.add_argument("--path", default="", help="explicit output path")
    args = ap.parse_args(argv)
    record = read_room_record(args.root, args.project)
    text = build(record)
    check = verify(text)
    out = {"slug": record["slug"], "messages": record["count"],
           "redactions": record["redactions"], **check}
    if args.dry_run:
        print(json.dumps(out, indent=1, ensure_ascii=False))
        return 0
    if args.path:
        from liljack_board import write_atomic as wa
        wa(Path(args.path), text)
        out["path"] = args.path
    else:
        out.update(write(record))
    print(json.dumps(out, indent=1, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
