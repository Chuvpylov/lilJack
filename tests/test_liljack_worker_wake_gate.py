#!/usr/bin/env python3
"""hb-question-integration: a question-blocked todo must not arm, nor vanish.

A todo parked on a question is BLOCKED for its owner, so the heartbeat must not
nudge it — but it is the room waiting on its lead, so it belongs in the lead's
batched digest, and an unanswered question accumulates strikes against the lead
via codex's question_strike (never a parallel ladder).
"""
import sys
import tempfile
import time
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))

import worker_wake as W  # noqa: E402

REAL_PICK, REAL_CANDIDATES = W.pick, W.candidates

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

def reset_state():
    W.STATE = Path(tempfile.mkdtemp(prefix="lj-wake-gate-")) / "state.json"

reset_state()

print("a question-blocked todo is gated out of the nudge set, not dropped")

# board still claims it is active, but the registry says it is blocked on a
# question — the gate (codex's task_gate) is authoritative.
rp = types.ModuleType("review_panel")
rp.room_scope = lambda: "2026-01-01T00:00:00Z"
rp.todos = lambda since=None: [
    {"agent": "codex", "task": "t1", "state": "active", "updated": "2026-01-01T00:00:00Z"}]
ll = types.ModuleType("lead_lifecycle"); ll.lookup = lambda *a, **k: {"state": "idle"}
lt = types.ModuleType("liljack_tasks")
lt.tasks = lambda open_only=False, **k: [
    {"id": "t1", "owner": "codex", "room": "r-1", "state": "blocked",
     "note": "Question q-x: how do I integrate?"}]
sys.modules.update({"review_panel": rp, "lead_lifecycle": ll, "liljack_tasks": lt})

W.candidates = lambda *a, **k: []
class DeadTmux:
    def sessions(self, project): return []

# ⚠ `opens` reaches pick() POSITIONALLY — pick(project, lookup, opens, ...) —
# so capturing **kwargs alone left `opens` absent and the nudge-set assertion
# below passed against ANY implementation. Caught 2026-09-10 while planting the
# closed-task guard; take the third positional argument.
captured = {}
W.pick = lambda *a, **k: captured.update(dict(k, opens=dict(a[2]))) or None
rep = W.tick("/proj", root=None, dry_run=True, tmux=DeadTmux())
check(captured.get("gated") == [{"owner": "codex", "task": "t1",
                                 "reason": "Question q-x: how do I integrate?"}],
      "the question-blocked todo is gated, carrying the question as its reason")
check("t1" not in captured.get("opens", {}).get("codex", []),
      "…and removed from the nudge set — its owner is never nudged about it")

print("gated todos surface in the lead's batched digest")
reset_state()
W.pick = REAL_PICK
PEOPLE = [{"id": "s-lead", "agent": "claude", "harness": "claude", "role": "lead"},
          {"id": "s-w1", "agent": "codex", "harness": "codex", "role": "worker"}]
W.candidates = lambda *a, **k: PEOPLE
idle = lambda sid, p, h: {"state": "idle"}
c = W.pick("/proj", idle, {}, gated=[{"owner": "codex", "task": "t1",
                                      "reason": "blocked on question q-x"}])
check(c and c["kind"] == "escalate"
      and any(e["owner"] == "codex" and e["task"] == "t1" and "question" in e["reason"]
              for e in c["escalations"]),
      "the gated todo appears in the lead's digest, so it does not vanish")

print("an unanswered question accumulates strikes against the lead")
reset_state()
rp2 = types.ModuleType("room_protocol")
strikes = []
rp2.question_digest = lambda store, project: [{"id": "q-x", "task": "t1", "strikes": 0}]
def q_strike(store, project, qid, attempt, reason):
    strikes.append((qid, attempt, reason))
    return {"id": qid, "strikes": 1}
rp2.question_strike = q_strike
rp2.reconcile = lambda store, project: {"errors": [], "notices": []}
sys.modules["liljack_app.room_protocol"] = rp2
ws2 = types.ModuleType("liljack_workspace")
class FakeStore:
    def close(self): pass
ws2.Workspace = lambda root=None: FakeStore()
sys.modules["liljack_workspace"] = ws2

rep = W.question_tick("/proj", workspace_root=None, dry_run=False, now=1000000.0)
check(rep["open_questions"] == 1 and len(strikes) == 1,
      "one open question is struck once against the lead")
# rate-limited: a second call inside COOLDOWN does not re-strike
before = list(strikes)
rep2 = W.question_tick("/proj", workspace_root=None, dry_run=False, now=1000000.0 + 30)
check(rep2["results"] == [] and strikes == before,
      "…and the strike is rate-limited, not fired every 5s tick")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
