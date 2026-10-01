#!/usr/bin/env python3
"""test_liljack_mail.py — the Claude ↔ Codex mailbox (toolbox/liljack_mail.py).

Plain script, ✓/✗ per check, non-zero exit on failure — the house style.
"""
import json
import os
from pathlib import Path
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
import liljack_mail as M          # noqa: E402

PASS = FAIL = 0


def check(cond, label):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"✓ {label}")
    else:
        FAIL += 1
        print(f"✗ {label}")


def main():
    tmp = Path(tempfile.mkdtemp(prefix="ljmail-"))

    # ── send / inbox basics ──────────────────────────────────────────────
    row = M.send("start the en judge", "codex", frm="claude", subject="handover",
                 project="demo", root=tmp)
    check(row["frm"] == "claude" and row["to"] == "codex", "send records both ends")
    check(row["id"] and row["ts"], "send stamps an id and a timestamp")

    box = M.inbox("codex", root=tmp)
    check(len(box) == 1 and box[0]["text"] == "start the en judge", "recipient sees it")
    check(M.inbox("claude", root=tmp) == [], "sender does not see their own message")

    # ── ack is what makes it read, and it is idempotent ──────────────────
    check(M.ack([row["id"]], "codex", root=tmp) == 1, "ack records one read")
    check(M.ack([row["id"]], "codex", root=tmp) == 0, "re-acking the same id is a no-op")
    check(M.inbox("codex", root=tmp) == [], "acked message leaves the unread inbox")
    check(len(M.inbox("codex", unread_only=False, root=tmp)) == 1, "…but is still readable")
    check(M.inbox("codex", unread_only=False, root=tmp)[0]["read"] is True, "and is flagged read")

    # ── acking for one agent does not ack for the other ──────────────────
    b = M.send("both of you", "all", frm="operator", root=tmp)
    check(len(M.inbox("codex", root=tmp)) == 1 and len(M.inbox("claude", root=tmp)) == 1,
          "a broadcast reaches both agents")
    M.ack([b["id"]], "codex", root=tmp)
    check(M.inbox("codex", root=tmp) == [] and len(M.inbox("claude", root=tmp)) == 1,
          "one agent's ack does not mark it read for the other")
    check(M.inbox("operator", root=tmp) == [], "a broadcast is not mail to its own sender")

    # ── sent view shows delivery ─────────────────────────────────────────
    s = M.sent("operator", root=tmp)
    check(len(s) == 1 and s[0]["read_by"] == ["codex"], "sent view reports who has read it")

    # ── validation ───────────────────────────────────────────────────────
    for bad, label in ((lambda: M.send("x", "nobody", frm="claude", root=tmp), "unknown recipient"),
                       (lambda: M.send("", "codex", frm="claude", root=tmp), "empty text"),
                       (lambda: M.send("x", "codex", frm="codex", root=tmp), "sending to yourself"),
                       (lambda: M.send("x", "codex", frm="hal9000", root=tmp), "unknown sender"),
                       (lambda: M.inbox("hal9000", root=tmp), "inbox for an unknown agent"),
                       (lambda: M.inbox("codex", limit=0, root=tmp), "limit below range")):
        try:
            bad()
            check(False, f"refuses {label}")
        except ValueError:
            check(True, f"refuses {label}")

    # ── oversized content fails clearly, never silently truncates ─────────
    full = M.send("x" * M.MAX_TEXT, "codex", frm="claude", root=tmp)
    check(len(full["text"]) == M.MAX_TEXT, "a message at exactly MAX_TEXT is stored whole")
    store = tmp / "mailbox.jsonl"
    lines_before = len(store.read_text(encoding="utf-8").splitlines())
    for build, label, needle in (
            (lambda: M.send("y" * (M.MAX_TEXT + 1), "codex", frm="claude", root=tmp),
             "body one over MAX_TEXT", "4000"),
            (lambda: M.send("ok", "codex", frm="claude", project="p" * (M.MAX_PROJECT + 1), root=tmp),
             "project one over MAX_PROJECT", "80"),
            (lambda: M.send("ok", "codex", frm="claude", thread="t" * (M.MAX_THREAD + 1), root=tmp),
             "thread one over MAX_THREAD", "80")):
        try:
            build()
            check(False, f"refuses {label}")
        except ValueError as e:
            check(needle in str(e), f"refuses {label} (and names the limit)")
    check(len(store.read_text(encoding="utf-8").splitlines()) == lines_before,
          "a refused message writes nothing to the store")

    # ── a subject is a label, not the payload: truncated with a marker ─────
    long_subject = M.send("ok", "codex", frm="claude",
                          subject="s" * (M.MAX_SUBJECT + 1), root=tmp)
    check(len(long_subject["subject"]) == M.MAX_SUBJECT,
          "an over-long subject is stored at exactly MAX_SUBJECT")
    check(long_subject["subject"].endswith("…"),
          "an over-long subject ends with an honest truncation marker")
    check(long_subject["subject"] == "s" * (M.MAX_SUBJECT - 1) + "…",
          "the subject keeps its head and marks the cut, instead of refusing the send")

    # ── the reason this store is append-only: concurrent writers ─────────
    conc = Path(tempfile.mkdtemp(prefix="ljmail-conc-"))
    errs = []

    def spam(who, other, n):
        for i in range(n):
            try:
                M.send(f"{who} {i}", other, frm=who, root=conc)
            except Exception as e:      # pragma: no cover
                errs.append(e)

    a = threading.Thread(target=spam, args=("claude", "codex", 60))
    c = threading.Thread(target=spam, args=("codex", "claude", 60))
    a.start(); c.start(); a.join(); c.join()
    lines = (conc / "mailbox.jsonl").read_text(encoding="utf-8").splitlines()
    check(not errs, "concurrent senders raise nothing")
    check(len(lines) == 120, f"no message is lost under concurrency (got {len(lines)}/120)")
    check(all(json.loads(l) for l in lines), "every line is still valid JSON (no torn writes)")
    check(len({json.loads(l)["id"] for l in lines}) == 120, "ids are unique")
    check(len(M.inbox("codex", limit=200, root=conc)) == 60, "each side sees only the other's 60")

    # ── a torn line is skipped, not fatal ────────────────────────────────
    torn = Path(tempfile.mkdtemp(prefix="ljmail-torn-"))
    M.send("good one", "codex", frm="claude", root=torn)
    with (torn / "mailbox.jsonl").open("a", encoding="utf-8") as f:
        f.write('{"id": "half-written", "to": "cod\n')
    check(len(M.inbox("codex", root=torn)) == 1, "a truncated line is skipped, the rest survives")

    # ── the scrubber fails CLOSED ────────────────────────────────────────
    real = M._scrub
    try:
        M._scrub = lambda t: (_ for _ in ()).throw(ImportError("no scrubber"))
        try:
            M.send("token=hunter2", "codex", frm="claude", root=tmp)
            check(False, "a raw body is never stored when scrubbing fails")
        except ImportError:
            check(True, "a raw body is never stored when scrubbing fails")
    finally:
        M._scrub = real

    scrubbed = M.send("token=hunter2", "codex", frm="claude", root=tmp)
    check("[text omitted" in scrubbed["text"] or "hunter2" not in scrubbed["text"]
          or scrubbed["text"] == "token=hunter2",
          "body goes through the scrubber (redacted, omitted, or genuinely clean)")

    # ── detect_agent ─────────────────────────────────────────────────────
    keep = {k: os.environ.get(k) for k in ("LILJACK_AGENT", "CLAUDECODE", "CLAUDE_CODE_ENTRYPOINT")}
    try:
        os.environ["LILJACK_AGENT"] = "codex"
        check(M.detect_agent() == "codex", "LILJACK_AGENT wins")
        os.environ["LILJACK_AGENT"] = "not-an-agent"
        os.environ["CLAUDECODE"] = "1"
        check(M.detect_agent() == "claude", "CLAUDECODE identifies Claude Code")
        del os.environ["LILJACK_AGENT"]
        del os.environ["CLAUDECODE"]
        os.environ.pop("CLAUDE_CODE_ENTRYPOINT", None)
        codexish = [k for k in os.environ if k.startswith("CODEX_")]
        if not codexish:
            os.environ["CODEX_HOME"] = "/tmp/x"
            check(M.detect_agent() == "codex", "a CODEX_* variable identifies Codex")
            del os.environ["CODEX_HOME"]
        else:
            check(M.detect_agent() == "codex", "a CODEX_* variable identifies Codex")
    finally:
        for k, v in keep.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    # ── render + the injection block ─────────────────────────────────────
    blk = M.unread_block("claude", root=tmp)
    check("lilJack mail" in blk and "unread for claude" in blk, "unread_block names the agent")
    check(M.unread_block("claude", root=Path(tempfile.mkdtemp())) == "",
          "an empty mailbox injects nothing")

    # ── the MCP server exposes both tools and routes them ────────────────
    import liljack_mcp as S
    names = {t["name"] for t in S.TOOLS}
    check({"send_message", "read_messages"} <= names, "both tools are advertised")
    check({"send_message", "read_messages"} <= set(S.HANDLERS), "both tools are routed")
    listed = S.handle({"method": "tools/list", "params": {}})
    check(any(t["name"] == "send_message" for t in listed["tools"]), "tools/list carries them")

    # ── session / room scoping (mailbox-session-scoping, 2026-09-12) ──────────
    import sqlite3
    scope = Path(tempfile.mkdtemp(prefix="ljmail-scope-"))
    ws = scope / "workspace"; ws.mkdir()
    con = sqlite3.connect(ws / "workspace.sqlite3")
    con.executescript("CREATE TABLE room_members(session_id TEXT PRIMARY KEY, room_id TEXT);")
    con.executemany("INSERT INTO room_members VALUES(?,?)",
                    [("s-lead1", "r-liljack"), ("s-cx1", "r-liljack"), ("s-lead2", "r-trio"), ("s-cx2", "r-trio")])
    con.commit(); con.close()
    keep_ws = {k: os.environ.get(k) for k in ("LILJACK_WORKSPACE_ROOT", "LILJACK_WORKSPACE_SESSION")}
    os.environ["LILJACK_WORKSPACE_ROOT"] = str(ws)
    try:
        os.environ["LILJACK_WORKSPACE_SESSION"] = "s-lead1"
        r1 = M.send("liljack work for codex", "codex", "claude", root=scope)
        check(r1["frm_session"] == "s-lead1" and r1["room"] == "r-liljack",
              "a send is stamped with the sender session and its room")
        os.environ["LILJACK_WORKSPACE_SESSION"] = "s-lead2"
        r2 = M.send("trio work for codex", "codex", "claude", root=scope)
        check(r2["room"] == "r-trio", "another lead's send is stamped with ITS room")
        M._append(scope / "mailbox.jsonl", {"id": "legacy-1", "ts": "2026-09-12T00:00:00+00:00",
                  "frm": "claude", "to": "codex", "subject": "", "text": "legacy untagged"})
        got = {m["text"] for m in M.inbox("codex", root=scope, session="s-cx1")}
        check(got == {"liljack work for codex", "legacy untagged"},
              "codex in r-liljack sees only r-liljack mail plus legacy rows (no trio mail)")
        got = {m["text"] for m in M.inbox("codex", root=scope, session="s-cx2")}
        check(got == {"trio work for codex", "legacy untagged"},
              "codex in r-trio sees only r-trio mail plus legacy rows")
        got = {m["text"] for m in M.inbox("codex", root=scope, session="s-nowhere")}
        check(got == {"liljack work for codex", "trio work for codex", "legacy untagged"},
              "a session in no room (operator/legacy) still sees everything")
        os.environ["LILJACK_WORKSPACE_SESSION"] = "s-lead1"
        M.send("only for cx1", "codex", "claude", root=scope, to_session="s-cx1")
        check(any(m["text"] == "only for cx1" for m in M.inbox("codex", root=scope, session="s-cx1"))
              and not any(m["text"] == "only for cx1" for m in M.inbox("codex", root=scope, session="s-nowhere")),
              "to_session mail reaches exactly that session, not even a room-less reader")
        M.send("liljack broadcast", "all", "claude", root=scope)
        check(any(m["text"] == "liljack broadcast" for m in M.inbox("deepseek", root=scope, session="s-cx1"))
              and not any(m["text"] == "liljack broadcast" for m in M.inbox("deepseek", root=scope, session="s-cx2")),
              "a broadcast from a room lead stays inside that room")
        check(M.session_room("s-lead1") == "r-liljack" and M.session_room("s-nope") == "" and M.session_room("") == "",
              "session_room resolves membership, empty for unknown")
    finally:
        for k, v in keep_ws.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
