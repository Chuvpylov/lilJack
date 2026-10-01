#!/usr/bin/env python3
"""lead_lifecycle: harness-aware idle lookup for the delivery pump.

The load-bearing property is NOT that idle is detected — it is that a busy or
unrecognised lead is NEVER reported idle. Submitting into a working agent
corrupts its turn; withholding a notification only delays one.
"""
import os
import json
import sqlite3
import sys
import tempfile
import time
from pathlib import Path
from datetime import datetime, timezone, timedelta
from unittest.mock import patch, Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "toolbox" / "liljack_app"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "toolbox"))
import lead_lifecycle as L
from liljack_workspace import Workspace

ok = fail = 0
def check(cond, name):
    global ok, fail
    if cond:
        ok += 1; print(f"  ok     {name}")
    else:
        fail += 1; print(f"  FAIL   {name}")


def unknown(value, code):
    return (value['state'] == L.UNKNOWN and value['reason_code'] == code
            and bool(value['reason']) and value['native'] is None
            and value['event'] != 'Stop')

def make_store(dirpath, rows):
    p = Path(dirpath) / "lifecycle.sqlite3"
    db = sqlite3.connect(p)
    db.execute("CREATE TABLE events (seq INTEGER PRIMARY KEY, recorded_at TEXT NOT NULL, "
               "session_id TEXT NOT NULL, cwd TEXT NOT NULL, event TEXT NOT NULL, payload TEXT NOT NULL)")
    for i, (sid, cwd, ev, ws) in enumerate(rows, 1):
        at = time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime())
        db.execute("INSERT INTO events VALUES (?,?,?,?,?,?)",
                   (i, at, sid, cwd, ev, '{"workspace_session": "%s"}' % ws))
    db.commit(); db.close()
    return p

# ── classification: the safety rule ──────────────────────────────────────
check(L.classify("Stop") == L.IDLE, "Stop is idle")
check(L.classify("SessionEnd") == L.ENDED, "SessionEnd is ended")
check(L.classify("PostToolUse") == L.BUSY, "PostToolUse is busy")
check(L.classify("SubagentStop") == L.BUSY, "SubagentStop is BUSY, not idle (a subagent is not the lead)")
check(L.classify("SomethingNewIn2027") == L.UNKNOWN, "an unrecognised event is unknown")
check(L.classify(None) == L.UNKNOWN and L.classify("") == L.UNKNOWN, "missing event is unknown")
check(all(L.classify(e) != L.IDLE for e in
          ["SubagentStop", "PreToolUse", "PostToolUse", "UserPromptSubmit", "SessionStart",
           "Interrupt", "PermissionRequest", "SomethingNewIn2027", None, ""]),
      "NOTHING except a turn boundary ever classifies idle")

# ── harness-aware paths ──────────────────────────────────────────────────
os.environ.pop("LILJACK_CODEX_STATE", None); os.environ.pop("LILJACK_OPENCODE_STATE", None)
os.environ["CODEX_HOME"] = "/tmp/cx"; os.environ["OPENCODE_HOME"] = "/tmp/oc"
check(str(L.store_path("codex")) == "/tmp/cx/liljack/lifecycle.sqlite3", "codex path honours CODEX_HOME")
check(str(L.store_path("opencode")) == "/tmp/oc/liljack/lifecycle.sqlite3", "opencode path honours OPENCODE_HOME")
os.environ["CLAUDE_HOME"] = "/tmp/cl"
check(str(L.store_path("claude")) == "/tmp/cl/liljack/lifecycle.sqlite3", "claude path honours CLAUDE_HOME")
os.environ["LILJACK_CLAUDE_STATE"] = "/tmp/cloverride"
check(str(L.store_path("claude")) == "/tmp/cloverride/lifecycle.sqlite3", "claude explicit override wins")
os.environ.pop("LILJACK_CLAUDE_STATE")
check(L.store_path(None) is None and L.store_path("future-harness") is None, "unknown harness has no store")
os.environ["LILJACK_CODEX_STATE"] = "/tmp/override"
check(str(L.store_path("codex")) == "/tmp/override/lifecycle.sqlite3", "explicit override wins")
os.environ.pop("LILJACK_CODEX_STATE")

# ── lookup against a real sqlite store ───────────────────────────────────
with tempfile.TemporaryDirectory() as d:
    cwd = os.path.realpath(d)
    p = make_store(d, [("native-1", cwd, "UserPromptSubmit", "s-lead"),
                       ("native-1", cwd, "Stop", "s-lead"),
                       ("native-2", cwd, "PostToolUse", "s-other")])
    r = L.lookup("s-lead", cwd, "codex", path=p)
    check(r is not None and r["native"] == "native-1", "resolves the exact native session")
    check(r["event"] == "Stop" and r["state"] == L.IDLE, "latest event wins and classifies idle")
    check(r["age"] >= 0, "age is computed")
    r2 = L.lookup("s-other", cwd, "codex", path=p)
    check(r2 is not None and r2["state"] == L.BUSY, "a different session is independently busy")
    check(unknown(L.lookup("s-nobody", cwd, "codex", path=p), 'session_unmapped'), "unmapped workspace session has an explicit unknown reason")
    check(unknown(L.lookup("s-lead", "/some/other/project", "codex", path=p), 'session_unmapped'), "wrong legacy project stays unknown")
    check(L.lookup("s-lead", cwd, "claude", path=p)["state"] == L.IDLE, "a claude lead resolves like any other harness")
    check(unknown(L.lookup("s-lead", cwd, "codex", path=Path(d) / "missing.sqlite3"), 'store_missing'), "missing file has an explicit reason")
    bad = Path(d) / "corrupt.sqlite3"; bad.write_text("not a database")
    check(unknown(L.lookup("s-lead", cwd, "codex", path=bad), 'store_unreadable'), "corrupt store is explicitly unknown, never raises")

# Managed identity is authoritative even after a cwd change. A second session
# of the same provider and a foreign project sharing its folder stay distinct.
with tempfile.TemporaryDirectory() as d:
    root = Path(d)
    project, folder, changed = root/'Project', root/'Room', root/'Changed'
    project.mkdir(); folder.mkdir(); changed.mkdir()
    w = Workspace(root/'workspace')
    try:
        sid = w.observe({'harness':'tmux','session':'lead','agent':'codex',
                         'project':str(project),'cwd':str(folder)})['id']
        peer = w.observe({'harness':'tmux','session':'peer','agent':'codex',
                          'project':str(project),'cwd':str(folder)})['id']
        p = make_store(root, [('native-a', str(changed), 'Stop', sid),
                              ('native-b', str(changed), 'PostToolUse', peer)])
        def lookup(target=sid, proj=project, harness='codex'):
            return L.lookup(target, proj, harness, path=p, workspace_root=w.root)
        check(lookup()['state'] == L.IDLE, 'managed session resolves by identity after cwd changes')
        check(lookup(peer)['state'] == L.BUSY, 'same-provider session keeps its separate busy state')
        check(unknown(lookup(proj=folder), 'project_mismatch'), 'shared folder never bypasses project scope')
        check(unknown(lookup(harness='claude'), 'harness_mismatch'), 'managed session cannot be relabelled as another harness')
        with sqlite3.connect(p) as db:
            db.execute('UPDATE events SET event=? WHERE session_id=?', ('SubagentStop','native-a'))
        check(lookup()['state'] == L.BUSY, 'cross-folder subagent completion never marks the lead idle')
        with sqlite3.connect(p) as db:
            db.execute('UPDATE events SET event=? WHERE session_id=?', ('FutureEvent','native-a'))
        check(lookup()['reason_code'] == 'unknown_event' and lookup()['state'] == L.UNKNOWN,
              'unrecognized event explains unknown without pretending to be idle')
        with sqlite3.connect(p) as db:
            db.execute('UPDATE events SET recorded_at=? WHERE session_id=?', ('bad-time','native-a'))
        check(unknown(lookup(), 'invalid_timestamp'), 'invalid timestamp is unknown with a reason')
        w.db.execute('UPDATE sessions SET data=? WHERE id=?', ('malformed',sid))
        check(unknown(lookup(), 'workspace_unreadable'), 'corrupt workspace mapping fails closed')
        check(unknown(L.lookup('', project, 'codex', path=p), 'missing_target'), 'missing target is explained')
        check(unknown(L.lookup(sid, project, 'unsupported'), 'unsupported_harness'), 'unsupported harness is explained')
    finally:
        w.close()

# Exercise the real pump/default lookup seam with an isolated workspace. No
# transport runs: the mock verifies both the wait and successful idle branches.
with tempfile.TemporaryDirectory() as d:
    from liljack_app.room_delivery import pump
    root = Path(d)
    project, folder = root/'Project', root/'Room'
    project.mkdir(); folder.mkdir()
    w = Workspace(root/'workspace')
    try:
        sid = w.observe({'harness':'tmux','session':'lead','agent':'codex',
                         'project':str(project),'cwd':str(folder)})['id']
        room = w.room_create(project, 'Delivery', folder=str(folder), actor='operator')['id']
        w.room_move(project,sid,room,actor='operator')
        w.room_lead(project,room,sid,actor='operator')
        message = w.post(project,'operator','Please review','delivery-test',destination=room)
        p = make_store(root, [('unrelated',str(folder),'Stop','s-another')])
        live = [{'id':sid,'agent':'codex','state':'running'}]
        send = Mock(return_value=Mock(returncode=0))
        with patch.dict(os.environ, {'LILJACK_CODEX_STATE':str(root)}):
            pump(w,project,live,send=send)
            state = w.db.execute('SELECT state,reason FROM room_delivery WHERE message_id=?',(message['id'],)).fetchone()
            check(state['state']=='pending' and 'No lifecycle event' in state['reason'],
                  'default pump leaves unknown pending and displays why')
            check(not send.called, 'unknown lifecycle never calls wake transport')
            at = (datetime.now(timezone.utc)-timedelta(seconds=3)).isoformat()
            with sqlite3.connect(p) as db:
                db.execute('INSERT INTO events VALUES(?,?,?,?,?,?)',
                           (2,at,'native-lead',str(folder),'PostToolUse',json.dumps({'workspace_session':sid})))
            pump(w,project,live,send=send)
            check(not send.called, 'busy cross-folder session never calls wake transport')
            with sqlite3.connect(p) as db:
                db.execute('UPDATE events SET event=? WHERE seq=2',('Stop',))
            pump(w,project,live,send=send)
            state = w.db.execute('SELECT state FROM room_delivery WHERE message_id=?',(message['id'],)).fetchone()[0]
            check(send.call_count==1 and state=='submitted',
                  'verified cross-folder idle lead submits once through the real pump')
    finally:
        w.close()

# ⚠ REGRESSION: a quiet lead must not fall out of view under worker traffic.
# The original SQL pre-filtered to the newest 400 events, so a busy worker could
# push the lead's last event out of the window and the lead read unknown forever
# — precisely the lead most in need of waking.
with tempfile.TemporaryDirectory() as d:
    cwd = os.path.realpath(d)
    rows = [("lead-native", cwd, "Stop", "s-quiet-lead")]
    rows += [("worker-native", cwd, "PostToolUse", "s-busy-worker")] * 900
    p2 = make_store(d, rows)
    r = L.lookup("s-quiet-lead", cwd, "codex", path=p2)
    check(r is not None and r["state"] == L.IDLE,
          "a quiet lead is still found behind 900 newer worker events (no recent-window truncation)")

# ⚠ verify_submit: a submit must VERIFY, not assert. Three distinct outcomes, and
# the caller must be able to tell a real wake from a held line from a dropped one.
_seq = [0]
def fake_lookup(target, project, harness, workspace_root=None, path=None):
    # `at` advances only when a new turn-start event "lands" (the caller flips _seq).
    return {"native": "n", "event": "UserPromptSubmit" if _seq[0] else "Stop",
            "age": 0.0, "at": f"2026-09-11T00:00:{_seq[0]:02d}",
            "state": L.BUSY if _seq[0] else L.IDLE, "reason": None, "reason_code": None}

with patch.object(L, "lookup", fake_lookup):
    _seq[0] = 0
    before = fake_lookup("t", "/p", "codex")
    _seq[0] = 1                       # a new event lands after the Enter
    outcome, _ = L.verify_submit("t", "/p", "codex", before, timeout=1.0, sleep=0.05)
    check(outcome == "delivered", "verify_submit reports delivered when a new event lands")

    _seq[0] = 1
    before = fake_lookup("t", "/p", "codex")     # already mid-turn
    outcome, _ = L.verify_submit("t", "/p", "codex", before, timeout=1.0, sleep=0.05)
    check(outcome == "queued", "verify_submit reports queued when the target was mid-turn and nothing new landed")

    _seq[0] = 0
    before = fake_lookup("t", "/p", "codex")     # idle, and it never starts
    outcome, _ = L.verify_submit("t", "/p", "codex", before, timeout=1.0, sleep=0.05)
    check(outcome == "unverified", "verify_submit reports unverified when an idle target never starts a turn")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
