#!/usr/bin/env python3
"""The registry's `held` park and `priority` rank (heartbeat-route-by-session).

`blocked` means "this cannot move"; `held` means "the assigner parked it". Before
`held` existed the lead parked a task with `block`, so it read as a dependency
forever. `priority` fixes the other half: the heartbeat offered the OLDEST task,
so a parked item could be handed out ahead of the work the room was on.

Registry-only: every write goes to a temp file, the live board is never touched.
"""
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
import liljack_tasks as T  # noqa: E402

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

P = Path(tempfile.mkdtemp(prefix="lj-held-")) / "agent_tasks.bmd"

# ── the state exists and is a PARK, not a dependency ─────────────────────
check("held" in T.STATES, "held is a lifecycle state")
check("held" in T.OPEN, "held is OPEN — it is not finished work")
check("held" not in T.PENDING, "held is NOT pending — it never arms the heartbeat")
check(all(s in T.TRANSITIONS for s in T.STATES), "every state declares its exits")
check("held" in T.TRANSITIONS.get("assigned", set()), "assigned → held is legal")
check("assigned" in T.TRANSITIONS.get("held", set()), "held → assigned is legal (to clear)")

# ── hold / unhold, and the assigner-only clear ───────────────────────────
t = T.propose("park me", "operator", owner="deepseek", priority=3, path=P)
check(t["state"] == "assigned" and t["priority"] == "3", "propose stores priority and assigns")
h = T.hold("park-me", "deepseek", "waiting for the operator", path=P)
check(h["state"] == "held", "the owner may park its own task")
check(not any(x["id"] == "park-me" for x in T.pending_tasks(path=P)),
      "a held task is NOT in pending_tasks (it cannot arm a nudge)")
check(any(x["id"] == "park-me" for x in T.tasks(open_only=True, path=P)),
      "…but it stays OPEN and visible — a park is not a delete")

try:
    T.set_state("park-me", "active", "deepseek", path=P)
    owner_cleared = True
except T.Denied:
    owner_cleared = False
check(not owner_cleared, "the OWNER may not clear a held task")
u = T.set_state("park-me", "active", "operator", path=P)
check(u["state"] == "active", "an ASSIGNER clears the park")

# ── priority ranks the listing (the nudge set consumes this order) ───────
T.propose("rank-low", "operator", owner="codex", priority=0, path=P)
T.propose("rank-high", "operator", owner="codex", priority=9, path=P)
ids = [x["id"] for x in T.tasks(owner="codex", open_only=True, path=P)]
check(ids[0] == "rank-high", "tasks() offers the high-priority task first, not the oldest")
check(T.set_priority("rank-low", 5, "operator", path=P)["priority"] == "5",
      "an assigner sets a priority")
try:
    T.set_priority("rank-high", 99, "codex", path=P)
    owner_ranked = True
except T.Denied:
    owner_ranked = False
check(not owner_ranked, "the OWNER may not rank its own task above the queue")
check(T.set_priority("rank-high", 0, "operator", path=P)["priority"] == "",
      "priority 0 restores the default order")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
