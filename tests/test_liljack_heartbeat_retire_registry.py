#!/usr/bin/env python3
"""The heartbeat does not retire while the DURABLE QUEUE still has work.

⚠ The defect, measured 2026-09-09: liljack-room-heartbeat.service exited
0/SUCCESS at 01:24:30 because `_no_work_left` consulted `review_panel.open_work()`
— the BOARD — and the board had gone all-done. FIVE MINUTES LATER five trio tasks
were opened in the REGISTRY. Nothing re-armed the service, so three agents sat
idle against a full queue and the operator had to notice by hand, twice.

The board's own standing note says which list is authoritative: "task-registry =
live — the trainer/data/braid/agent_tasks.bmd is the durable queue now; the mailbox
is chatter plus an importer." Retiring on an empty board while that queue is full
is the service concluding it has nothing to deliver on the strength of the one
list it does not deliver from.

Non-vacuity: the board is forced EMPTY in every case below, which is precisely the
state in which the old code retired. A test that left board work open would pass
against the buggy version too.
"""
import sys, pathlib, types

HERE = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "toolbox"))
sys.path.insert(0, str(HERE / "toolbox" / "liljack_app"))
import liljack_heartbeat as hb

ok = fail = 0
def check(cond, what):
    global ok, fail
    if cond: ok += 1; print(f"  ✓ {what}")
    else:    fail += 1; print(f"  ✗ {what}")

EMPTY_BOARD = {"open": 0}
NO_NOTICES  = {"delivery": {"pending": 0}}

def run(board, tasks, rep=NO_NOTICES, board_raises=False, tasks_raise=False):
    """Drive _no_work_left with the board and registry stubbed."""
    rp = types.ModuleType("review_panel")
    def _open_work():
        if board_raises: raise RuntimeError("board unavailable")
        return board
    rp.open_work = _open_work
    lt = types.ModuleType("liljack_tasks")
    def _tasks(open_only=False, **kw):
        if tasks_raise: raise RuntimeError("registry unavailable")
        return tasks
    lt.tasks = _tasks
    old = (sys.modules.get("review_panel"), sys.modules.get("liljack_tasks"))
    sys.modules["review_panel"], sys.modules["liljack_tasks"] = rp, lt
    try:
        return hb._no_work_left(rep)
    finally:
        for name, mod in zip(("review_panel", "liljack_tasks"), old):
            if mod is None: sys.modules.pop(name, None)
            else: sys.modules[name] = mod

print("heartbeat retire — the registry is the queue")

# THE REGRESSION. Empty board + open registry: the old code retired here.
r = run(EMPTY_BOARD, [{"id": "trio-relaunch", "state": "assigned"}])
check(r is None, "empty board + OPEN registry task -> KEEPS RUNNING (old code retired)")

# Genuinely finished: both empty, no notices.
r = run(EMPTY_BOARD, [])
check(r is not None, "empty board + empty registry + no notices -> retires")
check("registry" in (r or ""), f"the retire reason names the registry ({r!r})")

# A pending notice alone is enough to stay alive, registry or not.
r = run(EMPTY_BOARD, [], rep={"delivery": {"pending": 3}})
check(r is None, "pending notices alone keep it running")

# Open BOARD work still keeps it alive — the old behaviour must not regress.
r = run({"open": 2}, [])
check(r is None, "open board work keeps it running (unchanged)")

# ⚠ Silence is never a reason to retire: an unreadable registry must NOT be
# read as "no work". This is the failure mode that would reintroduce the bug.
r = run(EMPTY_BOARD, [], tasks_raise=True)
check(r is None, "registry unreadable -> keeps running, never retires on ignorance")
r = run(EMPTY_BOARD, [], board_raises=True)
check(r is None, "board unreadable -> keeps running")

# Several open tasks, and blocked ones still count as open work in the queue.
r = run(EMPTY_BOARD, [{"id": "a", "state": "blocked"}, {"id": "b", "state": "active"}])
check(r is None, "blocked + active registry tasks keep it running")

print(f"\n{ok} passed, {fail} failed")
sys.exit(1 if fail else 0)
