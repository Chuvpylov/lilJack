#!/usr/bin/env python3
"""An agent whose terminal came BACK must be nudged again.

⚠ THE DEFECT, MEASURED LIVE 2026-09-10. `mark_unavailable` was one-way. It was
set the moment tmux discovery missed a session, and NOTHING ever cleared it —
while pick() skips an unavailable agent permanently ("never retried"). So a
single blip in discovery removed an agent from the heartbeat for good.

deepseek carried "no live terminal" from 11.5 hours earlier while his pane was
live and idle with an open task, and every tick reported "no idle worker with
fresh open work". the operator reported the heartbeat as dead. It was dead FOR HIM,
and for exactly one agent, which is why it looked intermittent rather than
broken.

⚠ THE FIX IS NOT "CLEAR EVERY MARK". FATAL says stop nudging NOW; it does not
say forever. Exactly one reason ends in a way we can OBSERVE — a missing
terminal comes back and its pane is in tmux. Credit, quota, billing and auth
are not fixed by a pane existing, and re-nudging into one burns the strike
ladder for nothing. Recoverable is its own list.

Non-vacuity: every case has an IDLE owner with an OPEN task — the state in
which pick() is supposed to nudge — so a test that could not tell the fix from
its absence would have to pass in both directions, and the last case checks the
opposite direction explicitly.
"""
import sys, pathlib, tempfile

HERE = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "toolbox"))
sys.path.insert(0, str(HERE / "toolbox" / "liljack_app"))
import worker_wake as ww

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

ww.STATE = pathlib.Path(tempfile.mkdtemp(prefix="lj-recover-")) / "state.json"

PEOPLE = [{"id": "s-lead", "agent": "claude", "harness": "tmux", "role": "lead"},
          {"id": "s-w1",   "agent": "codex",  "harness": "tmux", "role": "worker"}]
IDLE = lambda sid, project, harness: {"state": "idle", "event": "Stop"}

def pick(live):
    ww.candidates = lambda project, root=None, live=None: PEOPLE
    return ww.pick("proj", IDLE, {"codex": ["t-open"]}, live=live)

def owner_nudged(live):
    """⚠ `is None` IS THE WRONG QUESTION. An unavailable worker correctly
    produces an ESCALATION to the lead, so pick() returns a choice either way.
    What the fix is about is whether the OWNER gets nudged about their task."""
    c = pick(live)
    return bool(c and c.get("kind") == "owner" and c.get("agent") == "codex")

print("an unavailable agent whose terminal returns is nudged again")

check(owner_nudged({"s-lead", "s-w1"}),
      "baseline: an idle owner with open work is nudged")

ww.mark_unavailable("s-w1", "no live terminal")
check(not owner_nudged({"s-lead", "s-w1"}),
      "while marked unavailable the owner is NOT nudged (this half already worked)")

# THE REGRESSION: the terminal is back. tick() clears a RECOVERABLE mark; here
# we assert the classification that drives it, then the effect.
check(ww._is_recoverable("no live terminal"),
      "'no live terminal' is classified RECOVERABLE — its end is observable")
for fatal in ("no credit remaining", "quota exceeded", "billing problem",
              "authentication failed", "unauthorized", "bad api key"):
    if ww._is_recoverable(fatal):
        check(False, f"{fatal!r} must NOT be recoverable")
        break
else:
    check(True, "credit / quota / billing / auth are NOT recoverable by a live pane")

ww.clear_unavailable("s-w1")
check(owner_nudged({"s-lead", "s-w1"}),
      "once the mark is cleared the owner is nudged again (was: never, forever)")

# ⚠ THE OPPOSITE DIRECTION. A fatal mark must SURVIVE a live terminal, or the
# heartbeat re-nudges an agent that has no credit until its strikes run out.
ww.mark_unavailable("s-w1", "no credit remaining")
check(not owner_nudged({"s-lead", "s-w1"}),
      "a FATAL mark still suppresses the nudge even with a live terminal")
check(not ww._is_recoverable(ww.unavailable("s-w1")["reason"]),
      "...and is not classified recoverable, so tick() will not clear it")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
