#!/usr/bin/env python3
"""test_liljack_heartbeat.py — the Codex chat watcher (toolbox/liljack_heartbeat.py).

Plain script, ✓/✗ per check, non-zero exit on failure — the house style.

⚠ `codex queue` is NEVER called here. Every delivery goes through an injected
`deliver=` stub that records what it was handed, so the suite can assert the
body is forwarded VERBATIM without waking anybody's session. The lifecycle DB
is a fixture this file builds; the real one at ~/.codex/liljack is never opened.
"""
import json
import sqlite3
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
import liljack_heartbeat as H          # noqa: E402
import liljack_mail as M               # noqa: E402

PASS = FAIL = 0
CWD = str(ROOT)                        # a real path, so resolve() is a no-op


def check(cond, label):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"✓ {label}")
    else:
        FAIL += 1
        print(f"✗ {label}")


def now():
    return datetime.now(timezone.utc)


def ago(sec):
    return (now() - timedelta(seconds=sec)).isoformat()


def make_db(rows, path=None):
    """A lifecycle fixture with the real schema. rows = (at, session, cwd, event)."""
    p = Path(path or (tempfile.mkdtemp(prefix="ljhb-db-") + "/lifecycle.sqlite3"))
    p.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(p)
    con.executescript("""
        CREATE TABLE IF NOT EXISTS events (
            seq INTEGER PRIMARY KEY, recorded_at TEXT NOT NULL,
            session_id TEXT NOT NULL, cwd TEXT NOT NULL,
            event TEXT NOT NULL, payload TEXT NOT NULL);""")
    con.executemany("INSERT INTO events(recorded_at,session_id,cwd,event,payload) "
                    "VALUES(?,?,?,?,?)", [(a, s, c, e, "{}") for a, s, c, e in rows])
    con.commit()
    con.close()
    return p


class Spy:
    """Stand-in for `codex queue` and for mail.send."""

    def __init__(self, ok=True):
        self.calls = []
        self.mails = []
        self.ok = ok

    def deliver(self, session, text):
        self.calls.append((session, text))
        return {"ok": self.ok, "err": "" if self.ok else "no such thread"}

    def send(self, text, to, frm="", subject="", project="", thread="", root=None):
        self.mails.append({"to": to, "frm": frm, "subject": subject, "text": text})
        return {"id": "spy"}


def task(root, text, subject="TASK: do the thing", frm="claude"):
    return M.send(text, "codex", frm=frm, subject=subject, project="demo", root=root)


# 2026-09-05: a dead thread was chosen and `codex queue` reported success, so
# the task was acked and lost. An ended session must never be a target.
def test_ended_session_is_never_a_target():
    ended = {"state": "idle", "ended": True,  "seq": 999, "cwd": None, "session": "dead"}
    live  = {"state": "idle", "ended": False, "seq": 1,   "cwd": None, "session": "live"}
    check(H.pick_target([ended]) is None, "an ended session alone yields no target")
    got = H.pick_target([ended, live])
    check(got is not None and got["session"] == "live", "a live idle session is chosen over an ended one")
    check(H.pick_target([{"state": "busy", "ended": False, "seq": 2, "cwd": None, "session": "b"}]) is None,
          "a busy session is not a target")

def main():
    # ══ 1. idle vs busy vs blocked vs settling ═══════════════════════════
    rows = [{"seq": 1, "at": ago(500), "session": "s-busy", "cwd": CWD, "event": "PreToolUse"},
            {"seq": 2, "at": ago(400), "session": "s-idle", "cwd": CWD, "event": "Stop"},
            {"seq": 3, "at": ago(5), "session": "s-fresh", "cwd": CWD, "event": "Stop"},
            {"seq": 4, "at": ago(300), "session": "s-block", "cwd": CWD, "event": "PermissionRequest"},
            {"seq": 5, "at": ago(200), "session": "s-end", "cwd": CWD, "event": "SessionEnd"},
            {"seq": 6, "at": ago(100), "session": "s-int", "cwd": CWD, "event": "Interrupt"},
            {"seq": 7, "at": ago(150), "session": "s-sub", "cwd": CWD, "event": "SubagentStop"}]
    st = {s["session"]: s for s in H.session_states(rows, settle=60)}
    check(st["s-idle"]["state"] == "idle", "a settled Stop is idle")
    check(st["s-busy"]["state"] == "busy", "a PreToolUse is busy")
    check(st["s-fresh"]["state"] == "settling", "a Stop inside the settle delay is not yet idle")
    check(st["s-block"]["state"] == "blocked", "a newest PermissionRequest is blocked, not idle")
    check(st["s-end"]["state"] == "idle" and st["s-end"]["ended"] is True,
          "SessionEnd is a turn end and is flagged ended")
    check(st["s-int"]["state"] == "idle", "Interrupt is a turn end")
    check(st["s-sub"]["state"] == "busy",
          "SubagentStop is NOT a turn end (it carries the parent session id)")

    # the settle delay is a boundary, so test both sides of it
    b = {s["session"]: s for s in H.session_states(
        [{"seq": 1, "at": ago(59), "session": "a", "cwd": CWD, "event": "Stop"},
         {"seq": 2, "at": ago(61), "session": "b", "cwd": CWD, "event": "Stop"}], settle=60)}
    check(b["a"]["state"] == "settling" and b["b"]["state"] == "idle",
          "the settle delay draws the line at exactly `settle` seconds")

    # a permission request that is no longer the newest event is not blocking
    seq = H.session_states([{"seq": 1, "at": ago(300), "session": "x", "cwd": CWD,
                             "event": "PermissionRequest"},
                            {"seq": 2, "at": ago(200), "session": "x", "cwd": CWD,
                             "event": "PostToolUse"}], settle=60)
    check(seq[0]["state"] == "busy", "an older PermissionRequest does not block a moving session")

    stale = H.session_states([{"seq": 1, "at": ago(200000), "session": "old",
                               "cwd": CWD, "event": "Stop"}], settle=60)
    check(stale == [], "a session with no event for a day is not a live target")

    # ══ 2. target selection ══════════════════════════════════════════════
    tgt = H.pick_target(H.session_states(rows, settle=60), cwd_filter=CWD)
    check(tgt and tgt["session"] == "s-int",
          "the newest live idle session wins (ended ones rank last)")
    only_end = H.pick_target(H.session_states(
        [{"seq": 9, "at": ago(200), "session": "s-end", "cwd": CWD, "event": "SessionEnd"}],
        settle=60), cwd_filter=CWD)
    # ⚠ This check asserted the OPPOSITE until 2026-09-05 — it encoded the bug.
    # `codex queue` returned success against a SessionEnd thread, so the task
    # was acked and delivered nowhere. An ended session is not a candidate.
    check(only_end is None, "an ended session is never a candidate, even when it is the only one")
    check(H.pick_target(H.session_states(rows, settle=60), cwd_filter="/nowhere/else") is None,
          "the cwd filter refuses a session in another repo")
    check(H.pick_target([], cwd_filter=None) is None, "no sessions → no target")

    # ══ 3. only TASK:-prefixed mail is work ══════════════════════════════
    check(H.is_task({"subject": "TASK: run the judge"}), "TASK: subject is a task")
    check(H.is_task({"subject": "  TASK: padded"}), "leading whitespace is tolerated")
    check(not H.is_task({"subject": "task: lowercase"}), "the prefix is literal and case-sensitive")
    check(not H.is_task({"subject": "re: TASK: quoted reply"}), "the prefix must START the subject")
    check(not H.is_task({"subject": ""}), "no subject is not a task")
    check(not H.is_task({"subject": "status update", "text": "TASK: in the body"}),
          "TASK: in the BODY is not a task — the subject decides")

    # ══ 4. dangerous payload screening ═══════════════════════════════════
    danger = {
        "systemctl": "please run systemctl --user restart den7700",
        "systemd-run": "start it with systemd-run --user --unit=foo",
        "git commit": "then git commit -m 'wip'",
        "git push": "and git push origin main",
        "rm -rf": "clean up with rm -rf runs/tmp",
        "CUDA_VISIBLE_DEVICES set": "export CUDA_VISIBLE_DEVICES=0 before training",
        "trio-v5-walk-queue": "restart the trio-v5-walk-queue unit",
    }
    for name, body in danger.items():
        check(name in H.screen(body), f"refuses `{name}`")
    check(H.screen("rm -fr build") and H.screen("rm -Rf build"),
          "flag order and case do not smuggle rm past the screen")
    check(H.screen('export CUDA_VISIBLE_DEVICES="" first') == [],
          "CUDA_VISIBLE_DEVICES set to empty is allowed (that is the safe form)")
    check(H.screen("CUDA_VISIBLE_DEVICES=") == [], "an empty assignment is allowed")
    check(H.screen("read the trainer/scripts/walk.py and report the ranking key") == [],
          "an ordinary task passes clean")
    check(len({n for n, _ in H.DANGER}) == 7, "all seven patterns are declared")

    # ══ 5. end-to-end: a real delivery ═══════════════════════════════════
    root = Path(tempfile.mkdtemp(prefix="ljhb-"))
    db = make_db([(ago(400), "sess-1", CWD, "UserPromptSubmit"),
                  (ago(300), "sess-1", CWD, "Stop")])
    body = ("Read the trainer/scripts/walk_lib.py and report which function decides the\n"
            "ranking key, with the file:line. Do not change anything.\n\n"
            "  - second line, indented\n"
            "  - trailing detail")
    m1 = task(root, body)
    spy = Spy()
    rep = H.tick(root=root, db_path=db, cwd_filter=CWD, dry_run=False,
                 deliver=spy.deliver, send_mail=spy.send)
    check(rep["action"] == "delivered", f"an idle session with a queued task gets it ({rep['action']})")
    check(len(spy.calls) == 1 and spy.calls[0][0] == "sess-1", "delivered to the idle session id")

    # ── verbatim: the body is forwarded byte for byte ────────────────────
    sent_text = spy.calls[0][1]
    check(body in sent_text, "the body is forwarded VERBATIM — not paraphrased, not truncated")
    check(sent_text.endswith(body), "nothing is appended after the body")
    check(sent_text.count("\n") == body.count("\n") + 1, "exactly one envelope line is prepended")
    check(m1["id"] in sent_text.splitlines()[0], "the envelope line names the message id")

    # ── acked, and never delivered twice ─────────────────────────────────
    check(M.inbox("codex", root=root) == [], "a delivered task is acked out of the unread inbox")
    db2 = make_db([(ago(400), "sess-1", CWD, "UserPromptSubmit"),
                   (ago(300), "sess-1", CWD, "Stop"),
                   (ago(250), "sess-1", CWD, "UserPromptSubmit"),
                   (ago(200), "sess-1", CWD, "Stop")])
    rep2 = H.tick(root=root, db_path=db2, cwd_filter=CWD, dry_run=False, min_interval=0,
                  deliver=spy.deliver, send_mail=spy.send)
    check(rep2["action"] == "idle-no-task", "the same task is never delivered a second time")
    check(len(spy.calls) == 1, "…and nothing more reached codex queue")
    check(rep2["detail"] == "idle, no queued task", "an empty queue is the resting state")

    # the log recorded the delivery
    log = H.read_log(root)
    dels = [r for r in log if r.get("event") == "delivered"]
    check(len(dels) == 1 and dels[0]["message"] == m1["id"], "the delivery is in heartbeat.log")
    check(dels[0]["acked"] is True and dels[0]["session"] == "sess-1", "…with the ack and the session")
    check(all("ts" in r and r["ts"] for r in log), "every log row is timestamped")
    check(all(isinstance(json.loads(l), dict) for l in
              H.log_path(root).read_text().splitlines() if l.strip()),
          "heartbeat.log is valid JSONL")

    # ══ 6. conversation is not work ══════════════════════════════════════
    root2 = Path(tempfile.mkdtemp(prefix="ljhb-chat-"))
    M.send("how is the walk going?", "codex", frm="claude", subject="checking in", root=root2)
    spy2 = Spy()
    rep = H.tick(root=root2, db_path=db, cwd_filter=CWD, dry_run=False,
                 deliver=spy2.deliver, send_mail=spy2.send)
    check(rep["action"] == "idle-no-task", "an unread NON-task message is not delivered as work")
    check(rep["chat_waiting"] == 1 and rep["queue_depth"] == 0, "…it is reported as chat, not queue")
    check(spy2.calls == [], "nothing was pushed into the session")
    check(len(M.inbox("codex", root=root2)) == 1, "and the conversation stays unread for Codex")

    # ══ 7. oldest task first ═════════════════════════════════════════════
    root3 = Path(tempfile.mkdtemp(prefix="ljhb-order-"))
    first = task(root3, "first body", subject="TASK: one")
    second = task(root3, "second body", subject="TASK: two")
    spy3 = Spy()
    rep = H.tick(root=root3, db_path=db, cwd_filter=CWD, dry_run=False,
                 deliver=spy3.deliver, send_mail=spy3.send)
    check(rep["message"] == first["id"] and first["id"] != second["id"],
          "the OLDEST queued task is the one delivered")
    check(len(M.inbox("codex", root=root3)) == 1, "the newer task stays queued")

    # ══ 8. refusal: logged, mailed, not delivered ════════════════════════
    root4 = Path(tempfile.mkdtemp(prefix="ljhb-ref-"))
    bad = task(root4, "please run systemctl --user restart den7700 and report",
               subject="TASK: restart the den")
    spy4 = Spy()
    rep = H.tick(root=root4, db_path=db, cwd_filter=CWD, dry_run=False,
                 deliver=spy4.deliver, send_mail=spy4.send)
    check(rep["action"] == "refused", "a dangerous payload is refused")
    check(spy4.calls == [], "a refused task never reaches codex queue")
    refs = [r for r in H.read_log(root4) if r.get("event") == "refused"]
    check(len(refs) == 1 and refs[0]["message"] == bad["id"], "the refusal is logged")
    check(refs[0]["patterns"] == ["systemctl"], "…with the pattern that matched")
    check(any(m["to"] == "claude" and "refused" in m["subject"].lower() for m in spy4.mails),
          "the refusal is mailed to claude")
    check(any(H.NOTICE_MARK in m["text"] for m in spy4.mails),
          "a notice says it was written by the watcher, not by Codex")
    check(M.inbox("codex", root=root4) == [], "a refused task is acked so the queue advances")

    # every pattern refuses end to end, not only in the screen() unit
    for name, snippet in danger.items():
        r = Path(tempfile.mkdtemp(prefix="ljhb-d-"))
        task(r, f"step one\n{snippet}\nstep two", subject=f"TASK: {name}")
        s = Spy()
        rp = H.tick(root=r, db_path=db, cwd_filter=CWD, dry_run=False,
                    deliver=s.deliver, send_mail=s.send)
        check(rp["action"] == "refused" and s.calls == [],
              f"end to end, a task containing `{name}` is never delivered")

    # ══ 9. blocked on a permission prompt ════════════════════════════════
    root5 = Path(tempfile.mkdtemp(prefix="ljhb-block-"))
    task(root5, "harmless work", subject="TASK: harmless")
    dbb = make_db([(ago(400), "sess-b", CWD, "PreToolUse"),
                   (ago(300), "sess-b", CWD, "PermissionRequest")])
    spy5 = Spy()
    rep = H.tick(root=root5, db_path=dbb, cwd_filter=CWD, dry_run=False,
                 deliver=spy5.deliver, send_mail=spy5.send)
    check(rep["action"] == "no-idle-session", "a blocked session is not an idle one")
    check(rep.get("blocked") == "sess-b", "the blocked session is reported")
    check(spy5.calls == [], "nothing is delivered on top of a permission prompt")
    check(any("blocked" in m["subject"].lower() for m in spy5.mails), "…and claude is mailed")
    check([r for r in H.read_log(root5) if r.get("event") == "blocked"], "…and it is logged")
    check(len(M.inbox("codex", root=root5)) == 1, "the task stays queued while he is blocked")

    # the same block is not re-mailed every tick
    spy5b = Spy()
    H.tick(root=root5, db_path=dbb, cwd_filter=CWD, dry_run=False,
           deliver=spy5b.deliver, send_mail=spy5b.send)
    check(spy5b.mails == [], "the same blocked state is not re-mailed on the next tick")

    # ══ 10. kill switch ══════════════════════════════════════════════════
    root6 = Path(tempfile.mkdtemp(prefix="ljhb-stop-"))
    task(root6, "work that must not go out", subject="TASK: blocked by kill switch")
    H.stop_path(root6).parent.mkdir(parents=True, exist_ok=True)
    H.stop_path(root6).write_text("stop")
    spy6 = Spy()
    rep = H.tick(root=root6, db_path=db, cwd_filter=CWD, dry_run=False,
                 deliver=spy6.deliver, send_mail=spy6.send)
    check(rep["action"] == "stopped", "the kill switch stops the watcher")
    check("heartbeat.stop" in rep["detail"], "…and says which file did it")
    check(spy6.calls == [] and len(M.inbox("codex", root=root6)) == 1,
          "nothing delivered, nothing acked, while the kill switch exists")
    H.stop_path(root6).unlink()
    rep = H.tick(root=root6, db_path=db, cwd_filter=CWD, dry_run=False,
                 deliver=spy6.deliver, send_mail=spy6.send)
    check(rep["action"] == "delivered", "removing the kill switch resumes delivery")

    # ══ 11. budget caps ══════════════════════════════════════════════════
    # min interval — a delivery just happened above in root6
    rep = H.tick(root=root6, db_path=db2, cwd_filter=CWD, dry_run=False, min_interval=120,
                 deliver=spy6.deliver, send_mail=spy6.send)
    check(rep["action"] == "capped" and "min interval" in rep["detail"],
          "the minimum interval between deliveries is enforced")

    # no-advance: the session has not moved since the last delivery
    task(root6, "another one", subject="TASK: second")
    rep = H.tick(root=root6, db_path=db, cwd_filter=CWD, dry_run=False, min_interval=0,
                 deliver=spy6.deliver, send_mail=spy6.send)
    check(rep["action"] == "capped" and "not advanced" in rep["detail"],
          "a second task is not stacked on a session that has not woken yet")

    # per-run cap
    root7 = Path(tempfile.mkdtemp(prefix="ljhb-run-"))
    task(root7, "work", subject="TASK: run cap")
    spy7 = Spy()
    rep = H.tick(root=root7, db_path=db, cwd_filter=CWD, dry_run=False, min_interval=0,
                 max_per_run=3, run_count=3, deliver=spy7.deliver, send_mail=spy7.send)
    check(rep["action"] == "capped" and "per run" in rep["detail"], "the per-run cap fires")
    check(spy7.calls == [], "…and nothing is delivered at the cap")
    check(any("cap" in m["subject"].lower() for m in spy7.mails), "…and claude is told")
    rep = H.tick(root=root7, db_path=db, cwd_filter=CWD, dry_run=False, min_interval=0,
                 max_per_run=3, run_count=2, deliver=spy7.deliver, send_mail=spy7.send)
    check(rep["action"] == "delivered", "one below the cap still delivers")

    # per-hour cap, read back out of the log so a restart cannot reset it
    root8 = Path(tempfile.mkdtemp(prefix="ljhb-hour-"))
    task(root8, "work", subject="TASK: hour cap")
    for i in range(12):
        H.log_event({"event": "delivered", "message": f"old-{i}", "session": "sess-1", "seq": 1},
                    root=root8)
    spy8 = Spy()
    rep = H.tick(root=root8, db_path=db, cwd_filter=CWD, dry_run=False, min_interval=0,
                 max_per_hour=12, deliver=spy8.deliver, send_mail=spy8.send)
    check(rep["action"] == "capped" and "hour" in rep["detail"], "the per-hour cap fires")
    check(spy8.calls == [], "…and nothing is delivered")
    check(len(M.inbox("codex", root=root8)) == 1, "…and the task is not consumed by a cap")
    rep = H.tick(root=root8, db_path=db, cwd_filter=CWD, dry_run=False, min_interval=0,
                 max_per_hour=13, deliver=spy8.deliver, send_mail=spy8.send)
    check(rep["action"] == "delivered", "raising the hourly cap lets it through")

    # an hour-old delivery does not count against this hour
    check(len(H.deliveries(root8, since=now() + timedelta(seconds=1))) == 0,
          "the hourly window is a time window, not a total count")

    # ══ 12. dry run and failure handling ═════════════════════════════════
    root9 = Path(tempfile.mkdtemp(prefix="ljhb-dry-"))
    mdry = task(root9, "dry body", subject="TASK: dry")
    spy9 = Spy()
    rep = H.tick(root=root9, db_path=db, cwd_filter=CWD, dry_run=True,
                 deliver=spy9.deliver, send_mail=spy9.send)
    check(rep["action"] == "would-deliver", "--dry-run reports what it would do")
    check(spy9.calls == [], "…and never calls the delivery function")
    check(len(M.inbox("codex", root=root9)) == 1, "…and never acks the task")
    check("dry body" in rep["payload"], "…but shows the exact payload it would send")

    fail = Spy(ok=False)
    rep = H.tick(root=root9, db_path=db, cwd_filter=CWD, dry_run=False,
                 deliver=fail.deliver, send_mail=fail.send)
    check(rep["action"] == "deliver-failed", "a failed queue is reported as a failure")
    check(len(M.inbox("codex", root=root9)) == 1 and M.inbox("codex", root=root9)[0]["id"] == mdry["id"],
          "a failed delivery is NOT acked — the task stays queued")
    check(any("failed" in m["subject"].lower() for m in fail.mails), "…and claude is mailed")

    # ══ 13. missing lifecycle db ═════════════════════════════════════════
    rep = H.tick(root=Path(tempfile.mkdtemp(prefix="ljhb-nodb-")),
                 db_path="/nonexistent/lifecycle.sqlite3", cwd_filter=CWD, dry_run=False,
                 deliver=Spy().deliver)
    check(rep["action"] == "no-lifecycle", "a missing lifecycle db is reported, not fatal")

    # ══ 14. helpers ══════════════════════════════════════════════════════
    check(H.parse_ts("2026-09-05T09:30:57.400133+00:00").year == 2026, "ISO timestamps parse")
    check(H.parse_ts("2026-09-05T09:30:57Z").tzinfo is not None, "a Z suffix is UTC")
    check(H.parse_ts("not a date") is None and H.parse_ts(None) is None,
          "a bad timestamp is None, never a guess")
    check(H.parse_ts("2026-09-05T09:30:57").tzinfo == timezone.utc,
          "a naive timestamp is read as UTC")
    torn = Path(tempfile.mkdtemp(prefix="ljhb-torn-"))
    H.log_event({"event": "delivered", "message": "x"}, root=torn)
    with H.log_path(torn).open("a") as f:
        f.write('{"event": "half\n')
    check(len(H.read_log(torn)) == 1, "a torn log line is skipped, the rest survives")
    check("would-deliver" in H.summarise({"action": "would-deliver", "detail": "x",
                                          "queue_depth": 2}), "summarise renders an action")

    # ══ 15. the CLI wiring ═══════════════════════════════════════════════
    doc = H.__doc__ or ""
    check("systemd-run --user --unit=liljack-heartbeat --collect" in doc,
          "the docstring carries the exact start command")
    check("heartbeat.stop" in doc, "the docstring documents the kill switch")
    src = (ROOT / "toolbox" / "liljack_heartbeat.py").read_text()
    check("codex\", \"queue\", \"--thread\"" in src.replace("'", '"'),
          "the real delivery path is `codex queue --thread`")

    test_ended_session_is_never_a_target()

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
