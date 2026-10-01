#!/usr/bin/env python3
"""A task may declare how often it is worth asking about.

⚠ THE PROBLEM, MEASURED 2026-09-10. FRESH_TASK_S is ONE number for every task.
That is right for work somebody can advance on being asked, and wrong for a
WATCH task, whose whole job is to keep looking at something for hours and which
cannot be finished on demand. `trio-relaunch` monitors a 17-hour training run:
its owner reported "still healthy", aged out of the 20-minute window, and was
asked again — three times in one hour, each costing a turn to answer with the
same answer. backoff_for() softens that (15m/30m/60m/2h) but only after the
turns have already been spent.

⚠ AND WIDENING FRESHNESS ALONE ONLY MOVES THE NOISE. STALE_TASK_S (45m) would
then mark the same task stalled and ESCALATE it to the lead — the same
interruption through a different door. A declared cadence scales BOTH windows.

⚠ THIS IS NOT A MUTE, and the last two checks are what prove it: past its own
window the task is nudged and, further out, declared stale exactly as before.
"""
import sys, pathlib, tempfile, types, time
from datetime import datetime, timedelta, timezone

HERE = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "toolbox"))
sys.path.insert(0, str(HERE / "toolbox" / "liljack_app"))
import worker_wake as W

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

W.STATE = pathlib.Path(tempfile.mkdtemp(prefix="lj-cadence-")) / "state.json"

def windows(age_s, checkin):
    """Run the real tick() and report which window the task landed in."""
    when = (datetime.now(timezone.utc) - timedelta(seconds=age_s)).strftime("%Y-%m-%dT%H:%M:%SZ")
    rp = types.ModuleType("review_panel")
    rp.room_scope = lambda: "2026-01-01T00:00:00Z"
    rp.todos = lambda since=None: [
        {"agent": "claude", "task": "watch-me", "state": "active", "updated": when}]
    ll = types.ModuleType("lead_lifecycle"); ll.lookup = lambda *a, **k: {"state": "idle"}
    lt = types.ModuleType("liljack_tasks")
    lt.PENDING = ("proposed", "assigned", "active")
    row = {"id": "watch-me", "owner": "claude", "state": "active", "updated": when}
    if checkin is not None:
        row["checkin"] = str(checkin)
    lt.tasks = lambda open_only=False, **k: [row]
    proto = types.ModuleType("room_protocol"); proto.task_gate = lambda p, t, root=None: {"allowed": True}
    sys.modules.update({"review_panel": rp, "lead_lifecycle": ll,
                        "liljack_tasks": lt, "liljack_app.room_protocol": proto})
    class DeadTmux:
        def sessions(self, project): return []
    cap = {}
    real_pick, real_cand = W.pick, W.candidates
    W.candidates = lambda *a, **k: []
    W.pick = lambda p, l, o, **k: cap.update(fresh=set(k.get("fresh") or ()),
                                             stale=list(k.get("stale") or ()),
                                             opens=dict(o)) or None
    try:
        W.tick("/proj", root=None, dry_run=True, tmux=DeadTmux())
    finally:
        W.pick, W.candidates = real_pick, real_cand
    return cap

print("a task declares its own check-in rhythm")

# Without a declared cadence: 30 minutes is past FRESH (20m) and not yet STALE (45m).
c = windows(1800, None)
check("watch-me" not in c["fresh"], "no cadence: 30min is NOT fresh (the old behaviour)")
check(not c["stale"], "no cadence: 30min is not yet stale either")

# WITH a 4-hour cadence, the same 30-minute-old task is still fresh...
c = windows(1800, 4 * 3600)
check("watch-me" in c["fresh"], "cadence 4h: a 30min-old report is still fresh — no nudge")
check(not c["stale"], "...and NOT escalated as stale (widening freshness alone would have)")

# ⚠ NOT A MUTE. Past its own window it behaves exactly as any other task.
c = windows(5 * 3600, 4 * 3600)
check("watch-me" not in c["fresh"], "past its own window the task is nudgeable again")
check([t for _, t in c["stale"]] == ["watch-me"],
      "...and IS declared stale, so a genuinely abandoned watch still escalates")

# A malformed value must not become silence.
c = windows(1800, "not-a-number")
check("watch-me" not in c["fresh"],
      "a malformed checkin falls back to the default, never to going quiet")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
