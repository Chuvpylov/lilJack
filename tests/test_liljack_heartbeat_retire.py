#!/usr/bin/env python3
"""The heartbeat retires itself when there is no work left.

the operator's rule: a service that exists to wake people about work should not keep
ticking when there is no work. The interesting cases are the REFUSALS — an
unreadable board and still-queued notices must both keep it alive, because
stopping early strands the team with nothing able to wake them.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))
import liljack_heartbeat as hb
import review_panel as RP

ok = fail = 0
def check(cond, name):
    global ok, fail
    if cond: ok += 1; print(f"  ok     {name}")
    else:    fail += 1; print(f"  FAIL   {name}")

real = RP.open_work
def patch(fn):
    RP.open_work = fn
    sys.modules["review_panel"].open_work = fn


def patch_registry(rows):
    """Stub the DURABLE QUEUE too.

    ⚠ Added 2026-09-09. `_no_work_left` now also consults the task registry,
    because retiring on an empty BOARD while `agent_tasks.bmd` was full is what
    silently stopped the service on 2026-09-09 with five trio tasks queued.
    Without this stub the checks below read the REAL production registry, so
    they pass or fail according to whatever is queued at the moment — a
    non-hermetic suite that also cannot express "genuinely no work left".
    """
    import types
    m = types.ModuleType("liljack_tasks")
    m.tasks = lambda open_only=False, **kw: list(rows)
    sys.modules["liljack_tasks"] = m

try:
    patch(lambda *a, **k: {"open": 4, "error": None})
    check(hb._no_work_left({"delivery": {"pending": 0}}) is None, "open work -> keeps running")

    patch(lambda *a, **k: {"open": 0, "error": None})
    patch_registry([])          # "no work" means the QUEUE is empty too
    r = hb._no_work_left({"delivery": {"pending": 0}})
    check(isinstance(r, str) and r, "no work and no notices -> retires, with a stated reason")

    # ⚠ THE REGRESSION that stopped the service on 2026-09-09: an empty board
    # with a FULL queue must not retire.
    patch_registry([{"id": "trio-relaunch", "state": "assigned"}])
    check(hb._no_work_left({"delivery": {"pending": 0}}) is None,
          "empty board but an OPEN REGISTRY TASK -> keeps running")
    patch_registry([])

    check(hb._no_work_left({"delivery": {"pending": 2}}) is None,
          "notices still owed to the lead -> keeps running even with no board work")

    patch(lambda *a, **k: {"open": None, "error": "board unreadable"})
    check(hb._no_work_left({"delivery": {"pending": 0}}) is None,
          "UNKNOWN IS NEVER DONE: an unreadable board keeps it running")

    def boom(*a, **k): raise RuntimeError("board exploded")
    patch(boom)
    check(hb._no_work_left({"delivery": {"pending": 0}}) is None,
          "a raising predicate keeps it running and never crashes the loop")

    patch(real)
    live = RP.open_work()
    check(live.get("error") is None and isinstance(live.get("open"), int),
          "the real board answers with a count, not a guess")
finally:
    patch(real)

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
