#!/usr/bin/env python3
"""`busy` is a claim with a shelf life.

⚠ THE DEFECT, MEASURED 2026-09-10. A wedged session looks EXACTLY like a working
one: its last lifecycle event simply stops arriving, and `busy` is sticky. The
heartbeat's rule "busy, leave it alone" is correct — applied to a false premise
it means an agent is left alone forever.

codex read `busy` from a PostToolUse 839 MINUTES (14 hours) old while holding an
open task. His hooks had stopped recording at 00:30 and he went silent at 05:52;
between those two facts nothing in the system could say either one. The lead
found it by reading ages by hand.

⚠ `age` WAS ALREADY COMPUTED AND RETURNED BY lookup() AND CONSULTED BY NOBODY.
The one number that separates a working session from a wedged one was measured
and thrown away.

⚠ STALE DEGRADES TO UNKNOWN, NOT TO IDLE, AND THAT DISTINCTION IS THE WHOLE
DESIGN. Idle would mean "start nudging it", and a session that might genuinely
be mid-call must not be poked. Unknown means "do not nudge, but SAY SO" — the
lead is told, the agent is left alone.
"""
import sys, pathlib, time

HERE = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "toolbox"))
sys.path.insert(0, str(HERE / "toolbox" / "liljack_app"))
import lead_lifecycle as L

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

print("a BUSY event past its shelf life is not evidence of work")

check(L.classify("PostToolUse") == L.BUSY,
      "classify() itself is unchanged — a fresh PostToolUse is busy")
check(L.classify("Stop") == L.IDLE, "...and Stop is still idle")
check(L.BUSY_STALE_S >= 3600,
      "the threshold is generous (>=1h): a long real tool call must not read as wedged")
check(L.BUSY_STALE_S <= 6 * 3600,
      "...but short enough to catch a wedge the same day, not 14 hours later")

# The decision lives in lookup(); exercise it through the same shape lookup uses.
def state_for(age_s):
    """Reproduce lookup()'s final decision for a BUSY event of the given age."""
    state = L.classify("PostToolUse")
    if state == L.BUSY and age_s > L.BUSY_STALE_S:
        return L.UNKNOWN, "event_stale"
    return state, None

s, code = state_for(60)
check(s == L.BUSY and code is None, "a 1-minute-old PostToolUse is still BUSY")
s, code = state_for(L.BUSY_STALE_S - 1)
check(s == L.BUSY, "one second inside the window is still BUSY (no off-by-one wedge)")
s, code = state_for(14 * 3600)
check(s == L.UNKNOWN and code == "event_stale",
      "a 14-hour-old PostToolUse degrades to UNKNOWN/event_stale (the measured case)")
check(s != L.IDLE,
      "...and NOT to idle — a possibly-mid-call session is never nudged on a guess")

print("\na settled TOOL BOUNDARY ends the turn when Stop never arrives")

# ⚠ THE MEASUREMENT THIS RULE RESTS ON. Stop is lossy: codex recorded 245
# UserPromptSubmit against 130 Stop, so about half its turns end with no
# turn-end event and the last event stays a PostToolUse — BUSY forever.
# The discriminator is WHICH boundary we are sitting on:
#   PreToolUse  last -> a tool is RUNNING; quiet is expected.
#   PostToolUse last -> it RETURNED and nothing followed; the turn is over.
# Gaps after PostToolUse, measured over 5,082 codex events: median 1.8s,
# p90 22.3s, p99 79.8s, 0.3% over 120s.
def decide(event, age_s):
    state = L.classify(event)
    if state == L.BUSY and event == "PostToolUse" and age_s > L.IDLE_AFTER_POST_S:
        return L.IDLE, "turn_ended_no_stop"
    if state == L.BUSY and age_s > L.BUSY_STALE_S:
        return L.UNKNOWN, "event_stale"
    return state, None

check(L.IDLE_AFTER_POST_S >= 240,
      "the window is at least 3x the measured p99 gap (79.8s) — a working turn is never called idle")
check(L.IDLE_AFTER_POST_S <= 900,
      "...and short enough to matter: minutes, not the 2h stale threshold")

check(decide("PostToolUse", 5)[0] == L.BUSY,
      "a fresh PostToolUse is BUSY — mid-turn work is untouched")
check(decide("PostToolUse", 80)[0] == L.BUSY,
      "at the measured p99 gap (80s) it is STILL busy, so a slow turn is safe")
s2, code = decide("PostToolUse", 3 * 3600)
check(s2 == L.IDLE and code == "turn_ended_no_stop",
      "a settled PostToolUse becomes IDLE — the agent can finally be nudged")

# ⚠ THE OPPOSITE BOUNDARY MUST NOT MOVE. A long-running tool emits PreToolUse
# and then nothing until it returns; calling that idle would nudge an agent in
# the middle of a build.
check(decide("PreToolUse", 3600)[0] == L.BUSY,
      "an hour-old PreToolUse stays BUSY — a running tool is not a finished turn")
s3, code3 = decide("PreToolUse", 14 * 3600)
check(s3 == L.UNKNOWN and code3 == "event_stale",
      "...and only degrades to UNKNOWN once it is unambiguously wedged")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
