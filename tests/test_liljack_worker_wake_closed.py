#!/usr/bin/env python3
"""A task the REGISTRY has closed is never nudged, however the board reads it.

⚠ THE DEFECT, MEASURED LIVE 2026-09-10. The nudge set is a UNION of the board's
open rows with the registry's PENDING rows. The union exists for a good reason —
an item that lives ONLY on the board must stay nudgeable — but it could not tell
board-ONLY from board-STALE. A board row for a task the registry has marked
`done` is not board-only work; it is a view that has fallen behind the queue.

`room-phase-ui` was `done` in the registry and `active` on the board, and the
heartbeat nudged its owner to go and finish work that was already finished. Two
further rows (`room-phase-machine`, `room-question-routing`) were in the same
state. The board lag had its own cause, fixed at the source — the registry now
mirrors on every write rather than only on an MCP write — but a VIEW CAN ALWAYS
LAG THE STORE IT MIRRORS, so the nudge path needs its own guard.

⚠ The pre-existing gate loop could not have caught this: it iterates
`tasks(open_only=True)`, so a `done` task is never reached and can never be
removed from `opens` again.

Non-vacuity: the board in every case below says `active` for an idle owner —
exactly the state in which the buggy code nudged.
"""
import sys
import tempfile
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

W.STATE = Path(tempfile.mkdtemp(prefix="lj-wake-closed-")) / "state.json"


def opens_for(registry_rows, board_rows):
    """Drive the real tick() and capture the nudge set it builds."""
    rp = types.ModuleType("review_panel")
    rp.room_scope = lambda: "2026-01-01T00:00:00Z"
    rp.todos = lambda since=None: list(board_rows)

    ll = types.ModuleType("lead_lifecycle")
    ll.lookup = lambda *a, **k: {"state": "idle"}

    lt = types.ModuleType("liljack_tasks")
    lt.PENDING = ("proposed", "assigned", "active")
    lt.tasks = lambda open_only=False, **k: [
        r for r in registry_rows
        if not open_only or r["state"] not in ("done", "abandoned")]

    proto = types.ModuleType("room_protocol")
    proto.task_gate = lambda project, t, root=None: {"allowed": True}

    sys.modules.update({"review_panel": rp, "lead_lifecycle": ll,
                        "liljack_tasks": lt, "liljack_app.room_protocol": proto})

    class DeadTmux:
        def sessions(self, project): return []

    # ⚠ `opens` reaches pick() POSITIONALLY — pick(project, lookup, opens, ...).
    # Capturing **kwargs alone yields {} and every assertion about the nudge set
    # passes vacuously (test_liljack_worker_wake_gate.py did exactly that).
    captured = {}
    real_pick, real_candidates = W.pick, W.candidates
    W.candidates = lambda *a, **k: []
    W.pick = lambda *a, **k: captured.update(opens=a[2]) or None
    try:
        W.tick("/proj", root=None, dry_run=True, tmux=DeadTmux())
    finally:
        W.pick, W.candidates = real_pick, real_candidates
    return dict(captured.get("opens", {}))


BOARD_ACTIVE = [{"agent": "claude", "task": "room-phase-ui", "state": "active",
                 "updated": "2026-01-01T00:00:00Z"}]

print("the registry is the queue; the board is a view of it")

# THE REGRESSION. Registry: done. Board: active (stale mirror).
o = opens_for([{"id": "room-phase-ui", "owner": "claude", "state": "done"}],
              BOARD_ACTIVE)
check("room-phase-ui" not in o.get("claude", []),
      "a task the registry calls done is NOT nudged, though the board says active")

# The same, for an abandoned task — dropped work must not come back either.
o = opens_for([{"id": "room-phase-ui", "owner": "claude", "state": "abandoned"}],
              BOARD_ACTIVE)
check("room-phase-ui" not in o.get("claude", []),
      "an abandoned task is not nudged either")

# ⚠ THE GUARD MUST NOT EAT THE UNION IT GUARDS. A row that is genuinely
# board-ONLY — the registry has never heard of it — still has to be nudgeable,
# which is the whole reason the union exists.
o = opens_for([], BOARD_ACTIVE)
check("room-phase-ui" in o.get("claude", []),
      "a genuinely board-only row is STILL nudged — the union survives")

# And an open registry task is untouched.
o = opens_for([{"id": "room-phase-ui", "owner": "claude", "state": "active"}],
              BOARD_ACTIVE)
check("room-phase-ui" in o.get("claude", []),
      "an open task is nudged as before")

# Registry PENDING beyond the board's vocabulary still reaches the set.
o = opens_for([{"id": "t-assigned", "owner": "codex", "state": "assigned"},
               {"id": "room-phase-ui", "owner": "claude", "state": "done"}],
              BOARD_ACTIVE)
check(o.get("codex") == ["t-assigned"] and "room-phase-ui" not in o.get("claude", []),
      "assigned work still enters the set while the closed row stays out")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
