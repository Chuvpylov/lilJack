#!/usr/bin/env python3
"""No open room todo may sit undriven.

the operator, 2026-10-20: "why did lilJack stop driving claude and codex while the
Zirec room still shows unfinished todos? There should be no scenario where that
happens." Measured in room r-304d412f (snapshot seq 6292): 74 open rows, 20
claude rows bound to the stopped session s-fe687f66 and 47 codex rows to the
stopped s-9cb57664. The live claude worker s-63635b0d was refused on start
("belongs to session ..."), there was no rebind op, the heartbeat never nudged a
second session about an `active` row, an escalation with no live lead was
dropped, and a blocked-only queue retired the loop.

Each path below is a fixture that FAILED before the fix (the worker is refused,
the heartbeat rests, or the escalation vanishes) and after it reaches a live
owner or an explicit escalation, with the move logged in history.
"""
import json
import os
import sqlite3
import sys
import tempfile
import time
import types
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))

BASE = Path(tempfile.mkdtemp(prefix="lj-undriven-"))
os.environ["LILJACK_TASKS"] = str(BASE / "tasks.bmd")
os.environ["LILJACK_BOARD"] = str(BASE / "board.bmd")
os.environ.pop("LILJACK_WORKSPACE_SESSION", None)

import liljack_tasks as T          # noqa: E402
import worker_wake as W            # noqa: E402
import liljack_heartbeat as H      # noqa: E402

W.STATE = BASE / "wake-state.json"

ok = fail = 0


def check(cond, what):
    global ok, fail
    if cond:
        ok += 1
        print(f"  ok     {what}")
    else:
        fail += 1
        print(f"  FAIL   {what}")


def attempt(fn, *a, **k):
    """(result, exception) — a fixture must report FAIL, never crash the file."""
    try:
        return fn(*a, **k), None
    except Exception as exc:          # noqa: BLE001 — the exception IS the evidence
        return None, exc


PROJECT = "/proj"
RA, RB, RC = "r-aaaa", "r-bbbb", "r-cccc"   # RC is archived


def make_ws(sessions, members, leads=()):
    """sessions: [(sid, agent, state)]; members: {sid: room}; leads: [sid]."""
    d = Path(tempfile.mkdtemp(prefix="lj-undriven-ws-"))
    con = sqlite3.connect(d / "workspace.sqlite3")
    con.execute("CREATE TABLE sessions (id TEXT PRIMARY KEY, project TEXT, data TEXT)")
    con.execute("CREATE TABLE team (session_id TEXT PRIMARY KEY, role TEXT, parent_id TEXT)")
    con.execute("CREATE TABLE room_members (session_id TEXT, room_id TEXT)")
    con.execute("CREATE TABLE rooms (id TEXT PRIMARY KEY, state TEXT)")
    for rid in (RA, RB, RC):
        con.execute("INSERT INTO rooms VALUES(?,?)", (rid, "archived" if rid == RC else "active"))
    for sid, agent, state in sessions:
        con.execute("INSERT INTO sessions VALUES(?,?,?)", (sid, PROJECT, json.dumps(
            {"agent": agent, "harness": "tmux", "state": state, "project": PROJECT})))
        con.execute("INSERT INTO team VALUES(?,?,NULL)",
                    (sid, "lead" if sid in leads else "worker"))
    for sid, rid in members.items():
        con.execute("INSERT INTO room_members VALUES(?,?)", (sid, rid))
    con.commit()
    con.close()
    return d


def fresh_registry(name):
    p = BASE / f"{name}.bmd"
    os.environ["LILJACK_TASKS"] = str(p)
    return p


def as_session(sid):
    return patch.dict(os.environ, {"LILJACK_WORKSPACE_SESSION": sid})


def ops(task_id, path):
    return [(h.get("op"), h.get("by"), h.get("note") or "") for h in T.history(task_id, path=path)]


WS = make_ws(
    [("s-old", "claude", "stopped"), ("s-new", "claude", "running"),
     ("s-run", "claude", "running"), ("s-other", "claude", "running"),
     ("s-lead", "claude", "running"), ("s-cold", "codex", "stopped")],
    {"s-new": RA, "s-run": RA, "s-other": RB, "s-lead": RA, "s-old": RB, "s-cold": RB},
    leads=("s-lead",))
os.environ["LILJACK_WORKSPACE_ROOT"] = str(WS)

# ══ 1. a worker adopts its agent's row from a STOPPED session ═════════════
print("1. registry: a live worker may adopt a row bound to a stopped session")
p = fresh_registry("adopt")
a1 = T.propose("orphaned work", by="operator", owner="claude", room=RA, owner_session="s-old", path=p)
T.set_state(a1["id"], "active", "operator", path=p)          # it was being worked, then the session died
with as_session("s-new"):
    _, err = attempt(T.start, a1["id"], "claude", path=p)
row = T.get(a1["id"], path=p)
check(err is None, f"start from s-new is accepted (was: {type(err).__name__ if err else ''} {err or ''})")
check(row["owner_session"] == "s-new", f"the row is rebound to s-new ({row['owner_session']!r})")
check(any(o == "rebind" and "s-old" in n and "s-new" in n for o, _, n in ops(a1["id"], p)),
      "history logs the rebind with both sessions")

a2 = T.propose("live colleague's work", by="operator", owner="claude", room=RA, owner_session="s-run", path=p)
with as_session("s-new"):
    _, err = attempt(T.start, a2["id"], "claude", path=p)
check(isinstance(err, T.Denied), "a row bound to a RUNNING session stays refused")

a3 = T.propose("other room's orphan", by="operator", owner="claude", room=RA, owner_session="s-old", path=p)
with as_session("s-other"):
    _, err = attempt(T.start, a3["id"], "claude", path=p)
check(isinstance(err, T.Denied), "a session in a DIFFERENT room may not adopt it")

a4 = T.propose("unknown session's row", by="operator", owner="claude", room=RA, owner_session="s-ghost", path=p)
with as_session("s-new"):
    _, err = attempt(T.start, a4["id"], "claude", path=p)
check(isinstance(err, T.Denied), "a session the workspace does not know is NOT treated as dead")

a4b = T.propose("codex orphan", by="operator", owner="codex", room=RA, owner_session="s-cold", path=p)
with as_session("s-new"):
    _, err = attempt(T.start, a4b["id"], "claude", path=p)
check(isinstance(err, T.Denied), "another agent's orphan is not adoptable by claude")

# ══ 2. the room lead (or the operator) rebinds explicitly ═══════════════════════
print("2. registry: the room lead rebinds; a worker cannot")
a5 = T.propose("lead hands it on", by="operator", owner="claude", room=RA, owner_session="s-old", path=p)
with as_session("s-lead"):
    _, err = attempt(getattr(T, "rebind", None) or (lambda *a, **k: (_ for _ in ()).throw(
        AttributeError("liljack_tasks has no rebind"))), a5["id"], "s-new", "claude",
        note="s-old stopped", path=p)
check(err is None, f"the room lead's rebind is accepted ({err or ''})")
check(T.get(a5["id"], path=p)["owner_session"] == "s-new", "…and the row names s-new")
check(any(o == "rebind" for o, _, _ in ops(a5["id"], p)), "…and history logs it")

if hasattr(T, "rebind"):
    with as_session("s-new"):
        _, err = attempt(T.rebind, a2["id"], "s-new", "claude", path=p)
    check(isinstance(err, T.Denied), "a worker may not rebind a live colleague's row to itself")
    _, err = attempt(T.rebind, a5["id"], "s-other", "operator", path=p)
    check(isinstance(err, T.TaskError), "a rebind target outside the row's room is refused")
    _, err = attempt(T.rebind, a5["id"], "s-old", "operator", path=p)
    check(isinstance(err, T.TaskError), "a rebind target that is stopped is refused")
else:
    check(False, "a worker may not rebind (no rebind op exists)")
    check(False, "a target outside the room is refused (no rebind op exists)")
    check(False, "a stopped target is refused (no rebind op exists)")

# ══ 3. the heartbeat drives every orphan in one pass ═════════════════════
print("3. heartbeat: dead-session rows reach a live owner or the lead")
p = fresh_registry("drive")
o1 = T.propose("active on a dead session", by="operator", owner="claude", room=RA, owner_session="s-old", path=p)
T.set_state(o1["id"], "active", "operator", path=p)
o2 = T.propose("codex work, no codex left", by="operator", owner="codex", room=RA, owner_session="s-cold", path=p)
dep = T.propose("dependency", by="operator", owner="claude", room=RA, owner_session="s-new", path=p)
o3 = T.propose("waits on dependency", by="operator", owner="claude", room=RA, owner_session="s-new", path=p)
T.block(o3["id"], "operator", note="waits for dependency", blocked_on=dep["id"], path=p)
T.set_state(dep["id"], "done", "operator", path=p)            # finished with nothing running to release
o4 = T.propose("parked", by="operator", owner="claude", room=RA, owner_session="s-old", path=p)
T.hold(o4["id"], "operator", path=p)
o5 = T.propose("deepseek work, no deepseek here", by="operator", owner="deepseek", room=RA, path=p)
o6 = T.propose("blocked on a dead session", by="operator", owner="claude", room=RA, owner_session="s-old", path=p)
T.block(o6["id"], "operator", note="needs a rebuild", path=p)

o8 = T.propose("operator decides", by="operator", owner="operator", room=RA, path=p)
o7 = T.propose("parked room's orphan", by="operator", owner="claude", room=RC, owner_session="s-old", path=p)

live = {"s-new", "s-run", "s-lead", "s-other"}
drive = getattr(W, "drive_orphans", None)
rep, err = attempt(drive, PROJECT, root=WS, live=live, path=p) if drive else (None, AttributeError("no drive_orphans"))
check(err is None, f"the orphan pass runs ({err or ''})")
rep = rep or {}
check(T.get(o1["id"], path=p)["owner_session"] == "s-lead",
      f"o1 (active, dead session, no stream owner) reaches the live room LEAD ({T.get(o1['id'], path=p)['owner_session']!r})")
check(any(o == "rebind" and b == "heartbeat" for o, b, _ in ops(o1["id"], p)),
      "o1's rebind is logged as the heartbeat's")
esc = {e.get("task"): e for e in rep.get("orphans", [])}
check(o2["id"] in esc and "no live codex" in esc[o2["id"]].get("reason", ""),
      f"o2 (no live codex in the room) is escalated, naming the agent ({esc.get(o2['id'])})")
check(T.get(o3["id"], path=p)["state"] == "assigned", "o3 (blocked on a done dependency) is released")
check(T.get(o4["id"], path=p)["owner_session"] == "s-old", "o4 (held) is left alone")
check(o5["id"] in esc and "no live deepseek" in esc[o5["id"]].get("reason", ""),
      "o5 (agent with no session at all) is escalated")
check(T.get(o6["id"], path=p)["owner_session"] == "s-lead",
      "o6 (blocked, dead session) is rebound so its owner hears the lead's posts")
check(T.get(o7["id"], path=p)["owner_session"] == "s-old" and o7["id"] not in esc,
      "o7 (room archived by the operator) is neither rebound nor escalated")
check(o8["id"] not in esc, "o8 (owned by the operator, a human) is not escalated as an orphan")

# ══ 3b. the rebind target follows the row's stream ═══════════════════════
# Lead m-02ac7d2e (seq 6306): the pass sent 20 Zirec rows to a worker the operator had
# moved to lilJack. Order: same-stream owner, room lead, idle worker, busy worker.
print("3b. heartbeat: same-stream owner, then lead, then idle, then busy")
WS2 = make_ws(
    [("s-dead", "claude", "stopped"), ("s-lead2", "claude", "running"), ("s-idle", "claude", "running"),
     ("s-busy", "claude", "running"), ("s-same", "claude", "running")],
    {"s-lead2": RA, "s-idle": RA, "s-busy": RA, "s-same": RA, "s-dead": RB}, leads=("s-lead2",))
p = fresh_registry("stream")
busy = T.propose("other stream work", by="operator", owner="claude", room=RA, owner_session="s-busy",
                 task_id="zirec-k1-foo", path=p)
T.set_state(busy["id"], "active", "operator", path=p)
T.propose("asset library", by="operator", owner="claude", room=RA, owner_session="s-same",
          task_id="zirec-n1-assets", path=p)
q1 = T.propose("scene assembly", by="operator", owner="claude", room=RA, owner_session="s-dead",
               task_id="zirec-n3-scene", path=p)
q2 = T.propose("unrelated", by="operator", owner="claude", room=RA, owner_session="s-dead",
               task_id="zirec-x9-other", path=p)
q3 = T.propose("render thing", by="operator", owner="claude", room=RA, owner_session="s-dead",
               task_id="render-thing", brief="Depends on zirec-k1-foo landing first.", path=p)
live2 = {"s-lead2", "s-idle", "s-busy", "s-same"}
attempt(drive, PROJECT, root=WS2, live=live2, path=p) if drive else None
got = lambda t: T.get(t["id"], path=p)["owner_session"]
check(got(q1) == "s-same", f"q1 (zirec-n family) goes to the n-stream owner ({got(q1)!r})")
check(got(q3) == "s-busy", f"q3 (brief names zirec-k1-foo) goes to that row's owner ({got(q3)!r})")
check(got(q2) == "s-lead2", f"q2 (no stream owner) goes to the room lead, not a worker ({got(q2)!r})")
q4 = T.propose("lead gone", by="operator", owner="claude", room=RA, owner_session="s-dead",
               task_id="zirec-m7-misc", path=p)
attempt(drive, PROJECT, root=WS2, live=live2 - {"s-lead2"}, path=p) if drive else None
check(got(q4) == "s-idle", f"q4 (no lead live) goes to the IDLE worker, not the busy one ({got(q4)!r})")
check(got(busy) == "s-busy" and got(q2) == "s-lead2", "rows already on live sessions are not re-shuffled")

blip = T.propose("tmux blip", by="operator", owner="claude", room=RA, owner_session="s-run", path=p)
attempt(drive, PROJECT, root=WS, live={"s-new", "s-lead"}, path=p) if drive else None
check(T.get(blip["id"], path=p)["owner_session"] == "s-run",
      "a session the DB says is running is never rebound on a tmux miss")

# ══ 4. an escalation with no live lead goes to the operator ═════════════════════
print("4. heartbeat: no live lead -> the operator, not silence")
ws_nolead = make_ws([("s-w", "codex", "running")], {"s-w": RA})
W.STATE = BASE / "wake-state-4.json"
lookup = lambda sid, project, harness: {"state": "idle"}
choice, err = attempt(W.pick, PROJECT, lookup, {}, root=ws_nolead, live={"s-w"},
                      blocked=[{"owner": "codex", "task": "t-stuck", "reason": "blocked"}],
                      task_rooms={"t-stuck": RA})
check(err is None and choice and choice.get("kind") == "operator",
      f"pick() hands a lead-less escalation to the operator ({choice}, {err or ''})")

# An orphan from a room whose lead is gone must not land on ANOTHER room's lead.
ws_other = make_ws([("s-w", "codex", "running"), ("s-blead", "claude", "running")],
                   {"s-w": RA, "s-blead": RB}, leads=("s-blead",))
W.STATE = BASE / "wake-state-4b.json"
choice2, err = attempt(W.pick, PROJECT, lookup, {}, root=ws_other, live={"s-w", "s-blead"},
                       blocked=[{"owner": "deepseek", "task": "t-orph", "orphan": True,
                                 "reason": "no live deepseek session in room r-aaaa"}],
                       task_rooms={"t-orph": RA})
check(err is None and choice2 and choice2.get("kind") == "operator",
      f"an orphan of a lead-less room goes to the operator, not room B's lead ({(choice2 or {}).get('session')})")
W.STATE = BASE / "wake-state-4.json"

sent = []
if choice and choice.get("kind") == "operator":
    W._escalate_operator(None, choice["escalations"],
                      lambda text, to, subject="", root=None: sent.append((to, subject, text)),
                      headline=choice.get("headline"))
check(sent and sent[0][0] == "operator" and "t-stuck" in sent[0][2],
      "the the operator mail names the undriven task")

# ══ 5. a long-blocked queue keeps the loop alive ══════════════════════════
print("5. heartbeat: blocked rows nobody re-drove keep the loop running")


def no_work_left(registry):
    rp = types.ModuleType("review_panel")
    rp.open_work = lambda *a, **k: {"open": 0, "error": None}
    lt = types.ModuleType("liljack_tasks")
    lt.tasks = lambda open_only=False, **kw: list(registry)
    lt.PENDING = T.PENDING
    lt.BLOCKED_REDRIVE_S = getattr(T, "BLOCKED_REDRIVE_S", 6 * 3600)
    lt.needs_driving = getattr(T, "needs_driving", lambda rows, **k: [])
    old = (sys.modules.get("review_panel"), sys.modules.get("liljack_tasks"))
    sys.modules["review_panel"], sys.modules["liljack_tasks"] = rp, lt
    try:
        return H._no_work_left({"delivery": {"pending": 0}})
    finally:
        for name, mod in zip(("review_panel", "liljack_tasks"), old):
            if mod is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = mod


old_ts = (datetime.now(timezone.utc) - timedelta(hours=10)).strftime("%Y-%m-%dT%H:%M:%SZ")
r = no_work_left([{"id": "stuck-10h", "state": "blocked", "updated": old_ts, "room": RA}])
check(r is None, f"a row blocked for 10 h keeps the loop running ({r!r})")
r = no_work_left([{"id": "stuck-now", "state": "blocked"}])
check(isinstance(r, str) and "BLOCKED" in r, "a freshly blocked row alone still rests (unchanged)")

# ══ 6. the lead's digest is deliverable ═══════════════════════════════════
# Live 2026-10-21: the Zirec digest was 20293 chars / 91 items (35 duplicates: a
# blocked row counted as gated AND blocked). `session send` refuses over 8000, so
# 101 sends failed in 6 h, and the age in each reason re-signed it every hour.
print("6. heartbeat: the lead digest fits one send and is signed without ages")
W.STATE = BASE / "wake-state-6.json"
note = "Blocked on an upstream prerequisite row that is itself blocked; " * 3


def big_queue(hours):
    gated = [{"owner": "codex", "task": f"zirec-x{i}", "reason": note} for i in range(60)]
    blocked = [{"owner": "codex", "task": f"zirec-x{i}",
                "reason": f"blocked {hours + i % 5}h — never re-driven, decide it: {note[:120]}"}
               for i in range(60)]
    return gated, blocked


rooms6 = {f"zirec-x{i}": RB for i in range(60)}
g6, b6 = big_queue(7)
c6, err = attempt(W.pick, PROJECT, lookup, {}, root=ws_other, live={"s-w", "s-blead"},
                  gated=g6, blocked=b6, task_rooms=rooms6)
check(err is None and c6 and c6.get("kind") == "escalate" and c6.get("session") == "s-blead",
      f"the big queue escalates to room B's lead ({(c6 or {}).get('kind')}, {err or ''})")
items6 = (c6 or {}).get("escalations") or []
ids6 = [e.get("task") for e in items6]
check(len(ids6) == len(set(ids6)) == 60, f"each task appears once in the digest ({len(ids6)} items)")
text6 = W.nudge_text(c6) if c6 else ""
check(0 < len(text6) <= 8000, f"the digest fits one session send ({len(text6)} chars)")
import re as _re
shown = len(set(_re.findall(r"- (zirec-x\d+) ", text6)))
more = int((_re.search(r"\+(\d+) more", text6) or [0, 0])[1])
check(shown and shown + more == 60, f"every task is listed or counted ({shown} shown + {more} more)")
huge = [{"owner": "codex", "task": f"zirec-y{i}", "room": RB, "reason": note} for i in range(300)]
th = W._digest_text(huge)
mh = _re.search(r"\+(\d+) more", th)
check(len(th) <= 8000 and mh and len(set(_re.findall(r"- (zirec-y\d+) ", th))) + int(mh[1]) == 300,
      f"300 items fit one send and the rest are counted ({len(th)} chars)")
g7, b7 = big_queue(8)
sig7 = W.digest_signature([{**e, "room": RB} for e in b7])
check(c6 and sig7 == c6.get("signature"), "an hour of ageing does not re-sign the digest")
if c6:
    W.remember_digest("s-blead", c6["signature"])     # tick records failed sends too
c7, err = attempt(W.pick, PROJECT, lookup, {}, root=ws_other, live={"s-w", "s-blead"},
                  gated=g7, blocked=b7, task_rooms=rooms6)
check(err is None and not (c7 and c7.get("kind") == "escalate"),
      f"the aged queue waits out the cooldown ({(c7 or {}).get('kind')})")

print(f"\n{ok} passed, {fail} failed")
sys.exit(1 if fail else 0)
