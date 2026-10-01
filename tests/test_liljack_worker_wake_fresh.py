#!/usr/bin/env python3
"""An owner is not nudged about work they are demonstrably ON.

⚠ The defect, observed live 2026-09-09: the ordinary owner nudge fired for ANY
open task of an idle agent, regardless of how recently the owner had advanced
it — staleness only ever gated the ESCALATION path. `trio-relaunch` monitors a
62-hour training run and was being reported on every few minutes, yet still drew
"you are idle and 'trio-relaunch' is still open" on every cooldown. At a 5-minute
tick that is roughly 750 nudges for work that is going fine, and noise on that
scale is how a real nudge stops being read.

A nudge asks "are you on this?". A progress update made minutes ago has already
answered it.

Non-vacuity: every case below has an IDLE owner with an OPEN task — precisely the
state in which the old code always nudged. A test that used a busy owner would
pass against the buggy version too.
"""
import sys, pathlib, time

HERE = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "toolbox"))
sys.path.insert(0, str(HERE / "toolbox" / "liljack_app"))
import worker_wake as ww

ok = fail = 0
def check(cond, what):
    global ok, fail
    if cond: ok += 1; print(f"  ✓ {what}")
    else:    fail += 1; print(f"  ✗ {what}")

PEOPLE = [{"id": "s-lead", "agent": "claude", "harness": "tmux", "role": "lead"},
          {"id": "s-w1",   "agent": "codex",  "harness": "tmux", "role": "worker"}]
IDLE = lambda sid, project, harness: {"state": "idle", "event": "Stop"}

def run(open_items, fresh=None, stale=None):
    ww.candidates = lambda project, root=None, live=None: PEOPLE      # deterministic roster
    ww._state = lambda *a, **k: {}                          # no cooldown history
    return ww.pick("proj", IDLE, open_items, fresh=fresh, stale=stale)

print("worker nudge — recent progress silences the poke")

# Baseline: an idle owner with open work IS nudged. This is the old behaviour and
# it must survive, or the fix has simply disabled nudging.
c = run({"codex": ["t-open"]})
check(c and c["task"] == "t-open", "idle owner with open work is still nudged")

# THE REGRESSION: the same task, freshly advanced, must NOT be nudged.
c = run({"codex": ["t-open"]}, fresh={"t-open"})
check(c is None, "a task advanced recently is NOT nudged (old code nudged it)")

# Freshness is per-task, not per-agent: stale work still gets through.
c = run({"codex": ["t-fresh", "t-stale"]}, fresh={"t-fresh"})
check(c and c["task"] == "t-stale", "a fresh task is skipped but a stale sibling is nudged")

# The lead's own long-running task — the trio-relaunch case exactly.
c = run({"claude": ["trio-relaunch"]}, fresh={"trio-relaunch"})
check(c is None, "the lead monitoring a live run is not poked about it")
c = run({"claude": ["trio-relaunch"]})
check(c and c["task"] == "trio-relaunch", "...but is poked once it goes quiet")

# Omitting `fresh` entirely keeps the old contract for existing callers.
c = ww.pick("proj", IDLE, {"codex": ["t-open"]})
check(c and c["task"] == "t-open", "callers that pass no `fresh` are unaffected")

# Escalation must not be silenced by freshness — a STALE task belongs to the lead
# and is a different question from "is the owner on it".
c = run({"codex": ["t-open"]}, fresh={"t-open"}, stale=[("codex", "t-stuck")])
check(c and c["kind"] == "escalate"
      and any(e["owner"] == "codex" and e["task"] == "t-stuck" for e in c["escalations"]),
      "a stalled task still escalates to the lead even when the owner has fresh work")


# ---- freshness must read the REGISTRY, not only the board ----
#
# ⚠ The board is a VIEW; the registry is the QUEUE. `todos()` reads the BOARD,
# whose row only moves when somebody calls board_update, while an owner
# recording real progress with task_update advances the REGISTRY. Measured
# 2026-09-09: trio-relaunch had registry 07:25:15 and a board row of 07:30Z
# written by an unrelated batch. Same board-vs-registry split that silently
# retired the heartbeat earlier the same day.
print()
print("worker nudge — freshness reads both records")

import types, time as _t
from datetime import datetime, timezone

def _iso(age_s):
    return datetime.fromtimestamp(_t.time() - age_s, timezone.utc).isoformat().replace("+00:00", "Z")

def tick_fresh(board_rows, registry_rows):
    """Run tick()'s freshness computation with both sources stubbed."""
    rp = types.ModuleType("review_panel")
    rp.room_scope = lambda: ""
    rp.todos = lambda since=None: board_rows
    ll = types.ModuleType("lead_lifecycle"); ll.lookup = lambda *a, **k: None
    lt = types.ModuleType("liljack_tasks"); lt.tasks = lambda open_only=False, **k: registry_rows
    old = {n: sys.modules.get(n) for n in ("review_panel", "lead_lifecycle", "liljack_tasks")}
    sys.modules.update({"review_panel": rp, "lead_lifecycle": ll, "liljack_tasks": lt})
    captured = {}
    real_pick = ww.pick
    real_candidates = ww.candidates
    ww.pick = lambda *a, **k: captured.update(k) or None
    ww.candidates = lambda *a, **k: []
    try:
        ww.tick("proj", dry_run=True)
        return captured.get("fresh", set())
    finally:
        ww.pick = real_pick
        ww.candidates = real_candidates
        for n, m in old.items():
            if m is None: sys.modules.pop(n, None)
            else: sys.modules[n] = m

board_stale = [{"agent": "claude", "task": "trio-relaunch", "state": "queued", "updated": _iso(30 * 60)}]
# ⚠ state="active" is REQUIRED here: these rows simulate an owner ADVANCING
# the task, and only `active` earns the quiet window. A row without it is
# an assignment, which must stay nudge-eligible.
reg_fresh   = [{"id": "trio-relaunch", "state": "active", "updated": _iso(60)}]

f = tick_fresh(board_stale, reg_fresh)
check("trio-relaunch" in f,
      "a STALE board row with FRESH registry progress counts as fresh (the real bug)")

f = tick_fresh(board_stale, [{"id": "trio-relaunch", "state": "active", "updated": _iso(30 * 60)}])
check("trio-relaunch" not in f,
      "stale in BOTH records is genuinely stale — the owner IS nudged")

f = tick_fresh([{"agent": "claude", "task": "trio-relaunch", "state": "queued", "updated": _iso(60)}],
               [{"id": "trio-relaunch", "state": "active", "updated": _iso(30 * 60)}])
check("trio-relaunch" in f, "a fresh BOARD row still counts, registry notwithstanding")

# An unreadable registry must degrade to board-only, never crash the tick.
rp_only = tick_fresh(board_stale, None)
check(isinstance(rp_only, set), "registry returning None does not crash the tick")


# ---- repeat nudges back off ----
#
# ⚠ A fixed cooldown treats "poked once" and "poked twenty times" alike. A
# long-running task cannot be finished on demand, so a flat 15-minute cooldown
# against a 20-minute freshness window puts its owner on a treadmill. Measured
# 2026-09-09: progress at 07:52:06, nudge at 08:12:09 — exactly 1200s, so the
# freshness test failed by ONE SECOND, twice running.
print()
print("worker nudge — repeat nudges back off")

check(ww.backoff_for(1) == ww.COOLDOWN_S, "the FIRST nudge keeps the prompt cooldown")
check(ww.backoff_for(2) == 2 * ww.COOLDOWN_S, "the second doubles")
check(ww.backoff_for(3) == 4 * ww.COOLDOWN_S, "the third doubles again")
check(ww.backoff_for(9) == 2 * 60 * 60, "it caps at 2h rather than growing forever")
check(ww.backoff_for(0) == ww.COOLDOWN_S, "an unseen task is not penalised")

# pick() must actually honour the widened window, not just compute it.
# (n=2 keeps it below STRIKES_ESCALATE, so the widening — not escalation — is what
# suppresses the nudge here.)
ww.candidates = lambda project, root=None, live=None: PEOPLE
now = time.time()
ww._state = lambda *a, **k: {"s-w1:t": now - 20 * 60, "s-w1:t:n": 2}   # 2 repeats -> 30m
c = ww.pick("proj", IDLE, {"codex": ["t"]}, now=now)
check(c is None, "a task nudged 2x is NOT re-nudged 20 min later (flat cooldown would have)")

ww._state = lambda *a, **k: {"s-w1:t": now - 20 * 60, "s-w1:t:n": 1}   # 1 repeat -> 15m
c = ww.pick("proj", IDLE, {"codex": ["t"]}, now=now)
check(c is not None, "...but a once-nudged task IS re-nudged at 20 min — stalls still surface")

# The counter must reset for a task left alone, or a stuck task stays quiet forever.
# ⚠ RESTORE the real _state first: the pick() checks above stubbed it with a
# lambda, and record() reads through the same function — leaving the stub in
# place made this check read a frozen dict and "fail" against correct code.
import json as _json, tempfile as _tf, importlib
importlib.reload(ww)
old_state = ww.STATE
ww.STATE = pathlib.Path(_tf.mkdtemp()) / "s.json"
ww.record("s-w1", "t", now=1000.0)
ww.record("s-w1", "t", now=1000.0 + 60)          # quick repeat -> counts up
n_up = _json.loads(ww.STATE.read_text())["s-w1:t:n"]
ww.record("s-w1", "t", now=1000.0 + 60 + 99999)  # long gap -> resets
n_reset = _json.loads(ww.STATE.read_text())["s-w1:t:n"]
ww.STATE = old_state
check(n_up == 2, f"consecutive nudges count up ({n_up})")
check(n_reset == 1, f"a long gap RESETS the count, so a stuck task returns to fast cadence ({n_reset})")


# ---- an ASSIGNMENT is not PROGRESS ----
#
# ⚠ A task handed out seconds ago has a timestamp as fresh as one somebody just
# advanced, and the two mean OPPOSITE things. Suppressing a brand-new assignment
# for 20 minutes is the failure the operator hit as "todo wakes!" — new work sitting
# silent because the anti-noise fix could not tell "just given to you" from
# "just worked on". `assigned` = accepted-but-not-started; only `active` earns
# the quiet window.
print()
print("worker nudge — an assignment is not progress")

now_iso = _iso(30)   # both rows are seconds old; only STATE differs
board = [{"agent": "codex", "task": "t-new", "state": "queued", "updated": _iso(30 * 60)}]

f = tick_fresh(board, [{"id": "t-new", "state": "assigned", "updated": now_iso}])
check("t-new" not in f,
      "a freshly ASSIGNED task is NOT protected — it is exactly what a nudge is for")

f = tick_fresh(board, [{"id": "t-new", "state": "active", "updated": now_iso}])
check("t-new" in f, "a freshly ADVANCED (active) task IS protected")

f = tick_fresh(board, [{"id": "t-new", "state": "proposed", "updated": now_iso}])
check("t-new" not in f, "a proposed task is not protected either")

# and an ACTIVE task that has gone quiet is still nudged
f = tick_fresh(board, [{"id": "t-new", "state": "active", "updated": _iso(40 * 60)}])
check("t-new" not in f, "an active task that went quiet loses its protection")

print(f"\n{ok} passed, {fail} failed")
sys.exit(1 if fail else 0)
