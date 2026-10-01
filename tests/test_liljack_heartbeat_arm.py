#!/usr/bin/env python3
"""The heartbeat ARMING path — the symmetric inverse of the self-retire.

the operator's rule, 2026-09-09: the heartbeat automatically starts when todos are
present and there is at least one PENDING; when there are no more cases all can
rest and it stops. Today only the STOP half existed (`_no_work_left` retires the
loop). This file pins the START half: `arm_decision` arms on a PENDING registry
task or a pending notice, and deliberately does NOT arm on `blocked` alone — a
blocked task is present but nobody can act on it, so a loop armed on it would
tick empty forever. Blocked still surfaces in the retire reason.
"""
import sqlite3
import sys
import tempfile
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))

import liljack_tasks as T      # noqa: E402
import liljack_heartbeat as H  # noqa: E402

ok = fail = 0


def check(cond, what):
    global ok, fail
    if cond:
        ok += 1
        print(f"  ✓ {what}")
    else:
        fail += 1
        print(f"  ✗ {what}")


def reg():
    d = Path(tempfile.mkdtemp(prefix="ljarm-"))
    return d / "agent_tasks.bmd"


def empty_ws():
    return Path(tempfile.mkdtemp(prefix="ljarm-ws-"))


def make_workspace(root, pending_notices):
    db = root / "workspace.sqlite3"
    db.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE room_delivery (message_id TEXT PRIMARY KEY, state TEXT)")
    for i in range(pending_notices):
        con.execute("INSERT INTO room_delivery VALUES(?,?)", (f"m-{i}", "pending"))
    con.commit()
    con.close()
    return db


def no_work_left(board, registry):
    """Drive _no_work_left with the board and registry stubbed."""
    rp = types.ModuleType("review_panel")
    rp.open_work = lambda *a, **k: board
    lt = types.ModuleType("liljack_tasks")
    lt.tasks = lambda open_only=False, **kw: list(registry)
    old_rp, old_lt = sys.modules.get("review_panel"), sys.modules.get("liljack_tasks")
    sys.modules["review_panel"], sys.modules["liljack_tasks"] = rp, lt
    try:
        return H._no_work_left({"delivery": {"pending": 0}})
    finally:
        for name, mod in zip(("review_panel", "liljack_tasks"), (old_rp, old_lt)):
            if mod is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = mod


print("heartbeat arm — starts on pending, never on blocked alone")

p = reg()
T.propose("do the thing", by="operator", owner="codex", path=p)
d = H.arm_decision(tasks_file=p, workspace_root=empty_ws())
check(d["arm"] is True and d["pending_tasks"] == 1, "a pending (assigned) task arms the heartbeat")

p2 = reg()
T.propose("unclaimed work", by="operator", path=p2)
d = H.arm_decision(tasks_file=p2, workspace_root=empty_ws())
check(d["arm"] is True, "a proposed task arms too — it is present and undone")

p3 = reg()
t = T.propose("stuck thing", by="operator", owner="codex", path=p3)
T.block(t["id"], by="codex", note="waiting on a rebuild", path=p3)
d = H.arm_decision(tasks_file=p3, workspace_root=empty_ws())
check(d["arm"] is False and d["blocked_tasks"] == 1 and d["pending_tasks"] == 0,
      "a BLOCKED task does not arm — it is present but not actionable")
check("blocked" in d["reason"], f"the rest reason names the blocked work ({d['reason']!r})")

p4 = reg()
ws = Path(tempfile.mkdtemp(prefix="ljarm-ws-"))
make_workspace(ws, 2)
d = H.arm_decision(tasks_file=p4, workspace_root=ws)
check(d["arm"] is True and d["pending_notices"] == 2,
      "pending room notices arm the heartbeat even with no registry task")

p5 = reg()
d = H.arm_decision(tasks_file=p5, workspace_root=empty_ws())
check(d["arm"] is False and d["pending_tasks"] == 0 and d["pending_notices"] == 0,
      "no pending tasks and no notices -> rest")

d = H.arm_decision(tasks_file=Path(tempfile.mkdtemp()),   # a directory, not a file
                   workspace_root=empty_ws())
check(d["arm"] is False and d["error"], "an unreadable registry reports an error, never a silent rest")

print("heartbeat retire — blocked-only rests but is never silent")

r = no_work_left({"open": 0, "error": None}, [{"id": "stuck", "state": "blocked"}])
check(isinstance(r, str) and r and "BLOCKED" in r and "registry" in r,
      f"blocked-only registry -> retires, naming blocked and registry ({r!r})")

r = no_work_left({"open": 0, "error": None}, [{"id": "a", "state": "active"}])
check(r is None, "a pending registry task still keeps the loop running")

def code_reload():
    """⚠ A LONG-LIVED LOOP RUNS THE MODULES IT IMPORTED AT START, FOREVER.

    Measured 2026-09-10: DeepSeek landed the fix that unblocked the whole room
    at 00:14 while this service had run since 00:00, so for fourteen minutes it
    executed the old module and nothing could tell. Codex reported it exactly:
    "running-service code version not verified". The loop now watches the files
    it actually imported and re-execs when they change.
    """
    import os, time
    base = H.code_stamps()
    check(any(f.endswith("liljack_tasks.py") for f in base),
          "the watch list is derived from sys.modules, not a hardcoded list")
    check(H.code_changed(dict(base)) is None, "an unchanged tree does not reload")

    # A module imported LATER must be adopted, never reported as a change —
    # worker_wake is imported inside the loop, so a naive baseline would
    # re-exec on the first tick, forever.
    watched = dict(base)
    sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))
    import worker_wake  # noqa: F401
    check(H.code_changed(watched) is None, "a late import is adopted, not reported as a change")
    check(any(f.endswith("worker_wake.py") for f in watched),
          "and it is watched from then on")

    # A file still being written is not yet a change: acting mid-write would
    # re-exec onto a half-saved module.
    target = [f for f in watched if f.endswith("worker_wake.py")][0]
    os.utime(target, None)
    check(H.code_changed(watched) is None, "a file inside the settle window is not a change")
    changed = H.code_changed(watched, now=time.time() + H.CODE_SETTLE_S + 1)
    check(changed == target, "once it settles, the changed file is named")

    # A vanished module counts as changed: replaced-in-place is the common case.
    gone = dict(watched); gone["/nonexistent/liljack_ghost.py"] = 1.0
    check(H.code_changed(gone) == "/nonexistent/liljack_ghost.py",
          "a module that disappeared counts as changed, not as unchanged")


def no_connection_leak():
    """⚠ `with sqlite3.connect(...)` DOES NOT CLOSE THE CONNECTION.

    The context manager governs the TRANSACTION, not the handle. Codex spotted
    the ResourceWarnings; in a loop that ticks every five seconds it is a leaked
    file descriptor per tick, in the one service that must never die. Four sites
    across the heartbeat and lead_lifecycle now use contextlib.closing.
    """
    import os, warnings
    with warnings.catch_warnings():
        warnings.simplefilter("error", ResourceWarning)
        H.arm_decision()                       # must not raise
    def fds():
        return len(os.listdir("/proc/self/fd"))
    H.arm_decision()
    before = fds()
    for _ in range(25):
        H.arm_decision()
    check(fds() <= before, f"file descriptors stay flat across 25 ticks ({before} -> {fds()})")


code_reload()
no_connection_leak()

print(f"\n{ok} passed, {fail} failed")
sys.exit(1 if fail else 0)
