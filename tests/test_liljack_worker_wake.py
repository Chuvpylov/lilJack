#!/usr/bin/env python3
"""The heartbeat wakes everyone, and escalates what has stalled.

the operator's rule: corresponding tasks go to their owner; STALE tasks go to the room
lead. The refusals matter most — a busy or unknowable session must never be
typed into, and no task may be re-sent inside its cooldown.
"""
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))
import worker_wake as W

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

PROJ = "/proj"
PEOPLE = [{"id": "s-lead", "agent": "claude", "harness": "claude", "role": "lead"},
          {"id": "s-w1", "agent": "codex", "harness": "codex", "role": "worker"},
          {"id": "s-w2", "agent": "deepseek", "harness": "opencode", "role": "worker"}]
W.candidates = lambda project, root=None, live=None: PEOPLE
W.STATE = Path("/tmp/liljack_wake_test.json")
if W.STATE.exists(): W.STATE.unlink()

idle = lambda sid, p, h: {"state": "idle"}
busy = lambda sid, p, h: {"state": "busy"}
unknown = lambda sid, p, h: None

# owner routing
c = W.pick(PROJ, idle, {"codex": ["fix-a"]})
check(c and c["agent"] == "codex" and c["task"] == "fix-a", "an owner is nudged about their OWN task")

# refusals
check(W.pick(PROJ, busy, {"codex": ["fix-a"]}) is None, "a BUSY session is never typed into")
check(W.pick(PROJ, unknown, {"codex": ["fix-a"]}) is None, "an UNKNOWABLE session is never typed into")
check(W.pick(PROJ, idle, {}) is None, "nobody is nudged when there is no open work")

# dedup
W.record("s-w1", "fix-a")
check(W.pick(PROJ, idle, {"codex": ["fix-a"]}) is None, "the same task is not re-sent inside the cooldown")
check(W.pick(PROJ, idle, {"codex": ["fix-a", "fix-b"]})["task"] == "fix-b",
      "a different task is still offered while one is cooling down")

# the lead is included, which it was not at first
c = W.pick(PROJ, idle, {"claude": ["lead-task"]})
check(c and c["session"] == "s-lead" and c["role"] == "lead",
      "the LEAD is nudged about its own work too — it is not exempt")

# escalation: stale work goes to the lead as ONE batched digest, naming the owner
if W.STATE.exists(): W.STATE.unlink()
c = W.pick(PROJ, idle, {"codex": ["fresh"]}, stale=[("deepseek", "stuck-task")])
check(c and c["kind"] == "escalate" and c["session"] == "s-lead",
      "a STALE task escalates to the room lead")
check(any(e["owner"] == "deepseek" and e["task"] == "stuck-task" for e in c["escalations"]),
      "…and names its owner in the digest")
check(W.pick(PROJ, busy, {"codex": ["fresh"]}, stale=[("deepseek", "stuck")]) is None,
      "escalation still respects idle — a busy lead is left alone")
W.record("s-lead", "digest")
c2 = W.pick(PROJ, idle, {"codex": ["fresh"]}, stale=[("deepseek", "stuck-task")])
check(c2 and c2["agent"] == "codex" and c2["kind"] == "owner",
      "once escalated, the loop falls back to ordinary owner nudges instead of repeating")

# ⚠ the live heartbeat demonstrated this on the lead itself: escalating the
# lead's OWN stale task back to the lead is a loop with extra words.
if W.STATE.exists(): W.STATE.unlink()
c = W.pick(PROJ, idle, {"codex": ["fresh"]}, stale=[("claude", "lead-own-stale")])
check(c is not None and c["task"] != "lead-own-stale",
      "the lead is NOT escalated its own stale task — the owner nudge covers it")

# ⚠ three unanswered nudges escalate, naming the owner and the count.
if W.STATE.exists(): W.STATE.unlink()
W.record("s-w1", "fix-a"); W.record("s-w1", "fix-a"); W.record("s-w1", "fix-a")
c = W.pick(PROJ, idle, {"codex": ["fix-a"]})
check(c and c["kind"] == "escalate", "3 unanswered nudges escalate to the lead")
check(any(e["owner"] == "codex" and e["task"] == "fix-a" and e["strikes"] == 3
          for e in c["escalations"]),
      "…naming the owner and the strike count")

# ⚠ an unavailable agent is escalated, not retried.
if W.STATE.exists(): W.STATE.unlink()
W.mark_unavailable("s-w1", "no live terminal")
c = W.pick(PROJ, idle, {"codex": ["fix-a"]})
check(c and c["kind"] == "escalate"
      and any(e["owner"] == "codex" and e["unavailable"] and "no live terminal" in e["reason"]
              for e in c["escalations"]),
      "an unavailable agent is escalated WITH its reason")
c2 = W.pick(PROJ, busy, {"codex": ["fix-a"]})
check(c2 is None, "…and the owner loop never re-nudges an unavailable agent")

# ⚠ a BLOCKED registry task must reach the lead's digest (the operator: spank the leader
# too) — it must not be nudged to its owner, and must not vanish.
if W.STATE.exists(): W.STATE.unlink()
c = W.pick(PROJ, idle, {}, blocked=[{"owner": "codex", "task": "blocked-task",
                                     "reason": "blocked: waiting on a decision"}])
check(c and c["kind"] == "escalate" and c["session"] == "s-lead",
      "a BLOCKED task surfaces in the lead's digest, not the owner's nudge")
check(any(e["owner"] == "codex" and e["task"] == "blocked-task" and "blocked" in e["reason"]
          for e in c["escalations"]),
      "…naming the owner and its blocked reason")

if W.STATE.exists(): W.STATE.unlink()
print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
