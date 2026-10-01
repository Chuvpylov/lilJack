#!/usr/bin/env python3
"""fix-cross-room-session-split: one agent session must not serve two rooms.

Rules (the operator 2026-09-12 via the trio lead):
  1. a room-stamped task is nudged only to a member of that room;
  2. a room-less task never lands on a session that sits in a room;
  3. an ACTIVE task never nudges a second session of its agent;
  4. task_propose from inside a room stamps room + owner_session;
  5. send_message reaches one exact session, even of the sender's own agent;
  6. starting a task stamps the starting session when the row had none.
"""
import json, os, sqlite3, sys, tempfile, types
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox")); sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))
import worker_wake as W  # noqa: E402
import liljack_mail as M  # noqa: E402

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

# a workspace store with two rooms, two codex sessions, two claude leads
root = Path(tempfile.mkdtemp(prefix="lj-split-")); (root).mkdir(exist_ok=True)
con = sqlite3.connect(root / "workspace.sqlite3")
con.executescript("""CREATE TABLE sessions(id TEXT PRIMARY KEY, project TEXT, data TEXT);
CREATE TABLE rooms(id TEXT PRIMARY KEY, project TEXT, name TEXT, collapsed INTEGER);
CREATE TABLE room_members(session_id TEXT PRIMARY KEY, room_id TEXT);
CREATE TABLE team(session_id TEXT PRIMARY KEY, role TEXT NOT NULL DEFAULT 'worker', parent_id TEXT);""")
for sid, agent in [("s-lead-a","claude"),("s-cx-a","codex"),("s-lead-b","claude"),("s-cx-b","codex"),("s-cx-free","codex")]:
    con.execute("INSERT INTO sessions VALUES(?,?,?)", (sid, "/p", json.dumps({"agent": agent, "state": "running"})))
con.executemany("INSERT INTO room_members VALUES(?,?)", [("s-lead-a","r-a"),("s-cx-a","r-a"),("s-lead-b","r-b"),("s-cx-b","r-b")])
con.executemany("INSERT INTO team VALUES(?,?,NULL)", [("s-lead-a","lead"),("s-lead-b","lead")])
con.commit(); con.close()

W.STATE = root / "wake.json"
PEOPLE = [{"id": "s-lead-a", "agent": "claude", "harness": "claude", "role": "lead"},
          {"id": "s-cx-a", "agent": "codex", "harness": "codex", "role": "worker"},
          {"id": "s-cx-b", "agent": "codex", "harness": "codex", "role": "worker"},
          {"id": "s-cx-free", "agent": "codex", "harness": "codex", "role": "worker"}]
W.candidates = lambda *a, **k: PEOPLE
idle = lambda sid, p, h: {"state": "idle"}

def nudged(opens, **kw):
    W.reset_room_cache()
    c = W.pick("/p", idle, opens, root=str(root), **kw)
    return c and (c["session"], c["task"])

print("1. a room task reaches only a member of that room")
check(nudged({"codex": ["t-a"]}, task_rooms={"t-a": "r-a"}) == ("s-cx-a", "t-a"),
      "task in r-a -> s-cx-a, never s-cx-b (listed later) nor s-cx-free")
PEOPLE2 = [p for p in PEOPLE if p["id"] != "s-cx-a"]
W.candidates = lambda *a, **k: PEOPLE2
check(nudged({"codex": ["t-a"]}, task_rooms={"t-a": "r-a"}) is None,
      "with r-a's codex gone, the r-a task is NOT handed to r-b's codex or a free codex")
W.candidates = lambda *a, **k: PEOPLE

print("2. a room-less task never lands in a room")
check(nudged({"codex": ["t-free"]}, task_rooms={"t-free": ""}) == ("s-cx-free", "t-free"),
      "room-less task -> the codex that is in no room")
W.candidates = lambda *a, **k: [p for p in PEOPLE if p["id"] != "s-cx-free"]
check(nudged({"codex": ["t-free"]}, task_rooms={"t-free": ""}) is None,
      "with no free codex, a room-less task is not delivered into any room")
W.candidates = lambda *a, **k: PEOPLE

print("3. an ACTIVE task never nudges a second session of its agent")
check(nudged({"codex": ["t-a"]}, task_rooms={"t-a": "r-a"}, task_states={"t-a": "active"},
             owner_sessions={"t-a": "s-cx-a"}) == ("s-cx-a", "t-a"),
      "active task with its session named -> that session only")
W.candidates = lambda *a, **k: PEOPLE2
check(nudged({"codex": ["t-x"]}, task_rooms={"t-x": ""}, task_states={"t-x": "active"},
             owner_sessions={"t-x": "s-cx-gone"}) is None,
      "active task whose session is gone -> nobody else of that agent is nudged")
check(nudged({"codex": ["t-y"]}, task_rooms={"t-y": ""}, task_states={"t-y": "active"}) is None,
      "active legacy task with NO session -> nobody (we cannot tell who started it)")
W.candidates = lambda *a, **k: PEOPLE

print("5. session-addressed mail between two leads of one agent")
keep = {k: os.environ.get(k) for k in ("LILJACK_WORKSPACE_ROOT", "LILJACK_WORKSPACE_SESSION")}
os.environ["LILJACK_WORKSPACE_ROOT"] = str(root); os.environ["LILJACK_WORKSPACE_SESSION"] = "s-lead-a"
mbox = root / "mail"
try:
    try:
        M.send("hi other me", "claude", "claude", root=mbox); check(False, "claude->claude without to_session is refused")
    except ValueError: check(True, "claude->claude without to_session is refused")
    row = M.send("hi other lead", "claude", "claude", root=mbox, to_session="s-lead-b")
    check(row["to_session"] == "s-lead-b" and row["frm_session"] == "s-lead-a" and row["room"] == "r-a",
          "claude->claude with to_session is stored with both sessions and the sender room")
    check([m["text"] for m in M.inbox("claude", root=mbox, session="s-lead-b")] == ["hi other lead"],
          "the other lead reads it")
    check(M.inbox("claude", root=mbox, session="s-lead-a") == [], "the sender does not see its own mail")
    check(M.room_sessions("r-a") and sorted(M.room_sessions("r-a")) == [("s-cx-a","codex"),("s-lead-a","claude")],
          "room_sessions lists members with agents")

    print("4. task_propose inside a room stamps room + owner_session (MCP layer)")
    import liljack_mcp as S
    captured = {}
    S.liljack_tasks = types.SimpleNamespace(
        propose=lambda *a, **k: captured.update(k) or {"id": "t", "state": "assigned", "owner": k.get("owner") or a[2], "title": a[0]},
        Denied=Exception, TaskError=Exception)
    S._mirror = lambda: None
    S.t_task_propose("x", owner="codex", agent="claude")
    check(captured.get("room") == "r-a" and captured.get("owner_session") == "s-cx-a",
          "owner codex from lead-a -> room r-a, owner_session = r-a's single codex")
    S.t_task_propose("y", owner="claude", agent="claude")
    check(captured.get("owner_session") == "s-lead-a", "owner == me -> my own session")
    S.t_task_propose("z", agent="claude")
    check(captured.get("room") == "r-a" and captured.get("owner_session") == "", "unowned proposal: room stamped, no session")

    print("6. starting a task stamps the starting session")
    import liljack_tasks as T
    tp = root / "tasks.bmd"
    r = T.propose("stamp me", "operator", owner="codex", path=tp)
    check((r.get("owner_session") or "") == "", "assigned by operator from outside a room: no session yet")
    os.environ["LILJACK_WORKSPACE_SESSION"] = "s-cx-b"
    r2 = T.start(r["id"], "codex", path=tp)
    check(r2.get("owner_session") == "s-cx-b", "codex s-cx-b starts it -> owner_session stamped s-cx-b")
finally:
    for k, v in keep.items():
        if v is None: os.environ.pop(k, None)
        else: os.environ[k] = v

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
