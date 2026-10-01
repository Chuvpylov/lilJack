#!/usr/bin/env python3
"""The heartbeat routes by EXACT SESSION, ranks by PRIORITY, and skips a PARK.

Pins the three registry tasks this exists for (heartbeat-route-by-session):
  (a) a task's owner_session wins over the agent slot — the misroute that sent
      visual-inspection-round-2 (owner_session s-3099f65f) to s-9ce113bb, another
      session of the same provider;
  (b) the nudge set is ranked by a lead-set priority, not just by age;
  (c) a `held` task is a deliberate park: it must never arm a nudge.

No workspace needed: candidates() and pick() take their inputs directly.
"""
import sys
import types
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

PROJ = "/proj"
# TWO deepseek sessions — the exact case the agent slot cannot tell apart.
PEOPLE = [
    {"id": "s-lead", "agent": "claude", "harness": "claude", "role": "lead"},
    {"id": "s-w1", "agent": "codex", "harness": "codex", "role": "worker"},
    {"id": "s-w2", "agent": "deepseek", "harness": "opencode", "role": "worker"},
    {"id": "s-w3", "agent": "deepseek", "harness": "opencode", "role": "worker"},
]
W.candidates = lambda project, root=None, live=None: PEOPLE
W.STATE = Path("/tmp/liljack_wake_route_test.json")
if W.STATE.exists(): W.STATE.unlink()
idle = lambda sid, p, h: {"state": "idle"}

# ── (a) EXACT SESSION over agent slot ────────────────────────────────────
print("(a) the task's exact session wins over the agent slot")
c = W.pick(PROJ, idle, {"deepseek": ["vision"]}, owner_sessions={"vision": "s-w3"})
check(c and c["session"] == "s-w3" and c["task"] == "vision",
      "owner_session=s-w3 routes the nudge to s-w3, not the first deepseek (s-w2)")
c = W.pick(PROJ, idle, {"deepseek": ["vision"]}, owner_sessions={"vision": "s-w2"})
check(c and c["session"] == "s-w2",
      "…and it follows the named session wherever it sits in the iteration")
c = W.pick(PROJ, idle, {"deepseek": ["vision"]}, owner_sessions={})
check(c and c["agent"] == "deepseek",
      "no owner_session -> the agent slot is the fallback (old behaviour)")
c = W.pick(PROJ, idle, {"deepseek": ["vision"]}, owner_sessions={"vision": "s-dead"})
check(c and c["agent"] == "deepseek",
      "a named session that is NOT live falls back to the agent slot")
# the misroute in the negative: two DIFFERENT tasks for the two deepseek sessions
c = W.pick(PROJ, idle, {"deepseek": ["for-w3", "for-w2"]},
           owner_sessions={"for-w3": "s-w3", "for-w2": "s-w2"})
check(c and c["session"] == "s-w2" and c["task"] == "for-w2",
      "the FIRST candidate is nudged only about ITS task, never the other session's")

# ── (b) PRIORITY before age ──────────────────────────────────────────────
print("(b) the nudge set is ranked by priority, then keeps the old order")
opens = {"deepseek": ["low", "high", "mid"]}
W.rank_tasks(opens, {"low": 0, "high": 5, "mid": 2})
check(opens["deepseek"] == ["high", "mid", "low"],
      "higher priority is offered first, regardless of arrival order")
opens = {"deepseek": ["old", "new"]}
W.rank_tasks(opens, {})
check(opens["deepseek"] == ["old", "new"],
      "with no ranks the order is unchanged (stable, default 0)")
opens = {"deepseek": ["a", "b", "c", "d"]}
W.rank_tasks(opens, {"a": 1, "b": 1, "c": 3, "d": 1})
check(opens["deepseek"] == ["c", "a", "b", "d"],
      "equal ranks keep their relative order (stable sort)")

# ── (c) a HELD task never arms a nudge ───────────────────────────────────
print("(c) a held registry task is skipped by the tick")
HIT = []

def fake_registry(rows):
    lt = types.ModuleType("liljack_tasks")
    lt.PENDING = ("proposed", "assigned", "active")
    lt.tasks = lambda open_only=False, **k: [dict(r) for r in rows]
    return lt

def fake_pick(project, lookup, open_items, **kw):
    HIT.append(({k: list(v) for k, v in open_items.items()}, kw.get("owner_sessions")))
    return None

W.pick = fake_pick
W.candidates = lambda project, root=None, live=None: PEOPLE
W._workspace = lambda root=None: Path("/nonexistent/workspace.sqlite3")

def run_tick(rows):
    HIT.clear()
    rp = types.ModuleType("review_panel")
    rp.room_scope = lambda: "2026-01-01T00:00:00Z"
    rp.todos = lambda since=None: []
    ll = types.ModuleType("lead_lifecycle"); ll.lookup = lambda *a, **k: {"state": "idle"}
    rpc = types.ModuleType("room_protocol")
    rpc.task_gate = lambda project, t, root=None: {"allowed": True}
    lt = fake_registry(rows)
    sys.modules.update({"review_panel": rp, "lead_lifecycle": ll,
                        "room_protocol": rpc, "liljack_tasks": lt})
    class Tmux:
        def sessions(self, project):
            return [{"id": p["id"], "agent": p["agent"], "state": "running"} for p in PEOPLE]
    return W.tick(PROJ, root=None, dry_run=True, tmux=Tmux())

ROWS = [
    {"id": "held-task", "owner": "deepseek", "owner_session": "s-w2",
     "state": "held", "priority": "9", "note": "parked by assigner"},
    {"id": "live-task", "owner": "deepseek", "owner_session": "s-w2",
     "state": "assigned", "priority": "0", "note": ""},
]
run_tick(ROWS)
opens = HIT[0][0] if HIT else {}
check("held-task" not in opens.get("deepseek", []),
      "a HELD task is not in the nudge set even at priority 9")
check("live-task" in opens.get("deepseek", []),
      "…while the ordinary task of the same owner is still offered")
check((HIT[0][1] or {}).get("live-task") == "s-w2",
      "the exact owner_session reaches pick() as well")

if W.STATE.exists(): W.STATE.unlink()
print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
