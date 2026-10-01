#!/usr/bin/env python3
"""heartbeat-room-lead-routing: gated/blocked escalations reach the ROOM lead.

Measured 2026-09-12 (the operator: "heartbeat is not spanking anyone"): the digest for
room r-22d217a2's six gated tasks went to the TEAM-TABLE lead s-ca1afb3b, an old
claude session, not to the room lead s-ddd6cac7 — and only after COOLDOWN_S when
that lead happened to be idle. Rules planted here:
  1. an escalation for a room-stamped task goes to that room's lead session;
  2. a task with no room (or no live room lead) falls back to the team lead;
  3. a digest whose CONTENT changed is sent at once, ignoring the cooldown;
  4. an unchanged digest waits COOLDOWN_S;
  5. a busy lead is not interrupted (the digest waits for idle).
"""
import sys, tempfile, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))
import worker_wake as W  # noqa: E402

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

W.STATE = Path(tempfile.mkdtemp(prefix="lj-wake-roomlead-")) / "state.json"
PEOPLE = [{"id": "s-teamlead", "agent": "claude", "harness": "claude", "role": "lead"},
          {"id": "s-roomlead", "agent": "claude", "harness": "claude", "role": "worker"},
          {"id": "s-w1", "agent": "codex", "harness": "codex", "role": "worker"}]
W.candidates = lambda *a, **k: PEOPLE
W.room_lead = lambda project, root, room: "s-roomlead" if room == "r-1" else None
idle = lambda sid, p, h: {"state": "idle"}
NOW = 1_000_000.0
gated = [{"owner": "codex", "task": "t1", "reason": "room phase REVIEW, state active; wait"}]

print("1. a room-stamped gated task escalates to the ROOM lead, not the team lead")
c = W.pick("/proj", idle, {}, gated=gated, task_rooms={"t1": "r-1"}, now=NOW)
check(c and c["kind"] == "escalate" and c["session"] == "s-roomlead" and c["role"] == "lead",
      f"digest goes to s-roomlead (got {c and c.get('session')})")
check(c and c.get("signature"), "the digest carries a content signature")

print("2. a task without a live room lead falls back to the team lead")
c2 = W.pick("/proj", idle, {}, gated=[dict(gated[0], task="t9")], task_rooms={"t9": "r-none"}, now=NOW)
check(c2 and c2["session"] == "s-teamlead", f"fallback to s-teamlead (got {c2 and c2.get('session')})")
c3 = W.pick("/proj", idle, {}, gated=[dict(gated[0], task="t8")], task_rooms={}, now=NOW)
check(c3 and c3["session"] == "s-teamlead", "standalone task also goes to the team lead")

print("3/4. unchanged digest waits the cooldown; a NEW item is sent at once")
W.remember_digest("s-roomlead", c["signature"], now=NOW)
again = W.pick("/proj", idle, {}, gated=gated, task_rooms={"t1": "r-1"}, now=NOW + 30)
check(again is None, "same digest 30s later is NOT re-sent")
later = W.pick("/proj", idle, {}, gated=gated, task_rooms={"t1": "r-1"}, now=NOW + W.COOLDOWN_S + 1)
check(later and later["session"] == "s-roomlead", "same digest after COOLDOWN_S is re-sent")
new_items = gated + [{"owner": "codex", "task": "t2", "reason": "blocked: needs the operator's trace log"}]
fresh = W.pick("/proj", idle, {}, gated=new_items, task_rooms={"t1": "r-1", "t2": "r-1"}, now=NOW + 30)
check(fresh and fresh["session"] == "s-roomlead" and len(fresh["escalations"]) == 2,
      "a digest with a NEW blocked item is sent 30s later, cooldown ignored")

print("5. a busy room lead is not interrupted")
busy = lambda sid, p, h: {"state": "busy"} if sid == "s-roomlead" else {"state": "idle"}
b = W.pick("/proj", busy, {}, gated=new_items, task_rooms={"t1": "r-1", "t2": "r-1"}, now=NOW + 60)
check(b is None, "busy lead: nothing sent this tick, digest waits for idle")

print("6. room_lead joins room_members with the TEAM table role (real sqlite); other leads in team are ignored")
import sqlite3, json
W.room_lead = W.__dict__["room_lead"] if False else None
import importlib; importlib.reload(W)
root = Path(tempfile.mkdtemp(prefix="lj-ws-")); db = root / "workspace.sqlite3"
con = sqlite3.connect(db)
con.executescript("""CREATE TABLE sessions(id TEXT PRIMARY KEY, project TEXT, data TEXT);
CREATE TABLE rooms(id TEXT PRIMARY KEY, project TEXT, name TEXT, collapsed INTEGER);
CREATE TABLE room_members(session_id TEXT PRIMARY KEY, room_id TEXT);
CREATE TABLE team(session_id TEXT PRIMARY KEY, role TEXT NOT NULL DEFAULT 'worker', parent_id TEXT);""")
con.execute("INSERT INTO sessions VALUES('s-a','/p',?)", (json.dumps({"state": "running"}),))
con.execute("INSERT INTO sessions VALUES('s-b','/p',?)", (json.dumps({"state": "running"}),))
con.execute("INSERT INTO sessions VALUES('s-c','/p',?)", (json.dumps({"state": "stopped"}),))
con.executemany("INSERT INTO team VALUES(?,?,NULL)", [("s-a", "worker"), ("s-b", "lead"), ("s-c", "lead"), ("s-old", "lead")])
con.execute("INSERT INTO rooms VALUES('r-1','/p','x',0)")
con.executemany("INSERT INTO room_members VALUES(?,?)", [("s-a", "r-1"), ("s-b", "r-1"), ("s-c", "r-2")])
con.commit(); con.close()
check(W.room_lead("/p", root, "r-1") == "s-b", "room r-1 lead is s-b")
check(W.room_lead("/p", root, "r-2") is None, "room r-2 has only a stopped lead -> None")
check(W.room_lead("/p", root, "") is None and W.room_lead("/p", root / "nope", "r-1") is None, "no room / no db -> None")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
