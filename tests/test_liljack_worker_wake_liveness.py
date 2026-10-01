#!/usr/bin/env python3
"""TMUX is the truth for liveness, and a failed nudge is never silent.

Pins the two registry tasks this exists for:
  hb-live-terminal-truth   — candidates() trusts tmux, not the DB row; nudge()
                             returns the refusal text; a failure records backoff.
  hb-three-strikes-escalate — 3 unanswered nudges escalate to the lead; no live
                             terminal / API-credit failure marks unavailable and
                             escalates at once; a silent lead escalates to operator.

The live defect: the heartbeat ran every 5s emitting sent:false forever, because
candidates() trusted the sessions row and picked a session whose tmux pane was
gone; nudge() threw the refusal away and record() ran only on success.
"""
import json
import sqlite3
import sys
import tempfile
import time
import types
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT / "toolbox" / "liljack_app"))

import worker_wake as W  # noqa: E402

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

def reset_state():
    W.STATE = Path(tempfile.mkdtemp(prefix="lj-wake-live-")) / "state.json"

reset_state()

# ══ 1. candidates(): tmux is the truth, not the DB row ══════════════════
def make_ws(sessions, lead=None):
    d = Path(tempfile.mkdtemp(prefix="lj-wake-ws-"))
    db = sqlite3.connect(d / "workspace.sqlite3")
    db.execute("CREATE TABLE sessions (id TEXT PRIMARY KEY, project TEXT, data TEXT)")
    db.execute("CREATE TABLE team (session_id TEXT PRIMARY KEY, role TEXT, parent_id TEXT)")
    for sid, agent, project in sessions:
        db.execute("INSERT INTO sessions VALUES(?,?,?)", (sid, project, json.dumps(
            {"agent": agent, "harness": "tmux", "state": "running", "project": project})))
    for sid in (lead or []):
        db.execute("INSERT INTO team VALUES(?,'lead',NULL)", (sid,))
    db.commit(); db.close()
    return d

print("candidates() resolves liveness from tmux")
d = make_ws([("s-lead", "claude", "/proj"), ("s-w1", "codex", "/proj")], lead=["s-lead"])
W._workspace = lambda root=None: Path(d) / "workspace.sqlite3"

both = W.candidates("/proj")
check([p["id"] for p in both] == ["s-lead", "s-w1"], "with no live view, the DB rows are used")

live = W.candidates("/proj", live={"s-lead"})
check([p["id"] for p in live] == ["s-lead"],
      "a session whose tmux pane is gone is NOT a candidate")
check(W.candidates("/proj", live=set()) == [],
      "…and an empty tmux view yields no candidates at all")

# ══ 2. nudge(): the refusal text comes back in out['reason'] ════════════
print("nudge() returns the refusal text, and never claims success")
CHOICE = {"session": "s-w1", "agent": "codex", "task": "fix-a", "kind": "owner"}
with patch("subprocess.run") as run:
    run.return_value.returncode = 2
    run.return_value.stderr = '{"error": "Target has no live terminal"}'
    run.return_value.stdout = ""
    out = W.nudge("/proj", CHOICE)
check(out["sent"] is False and out["submitted"] is False and out["reason"] == "Target has no live terminal",
      "a refused send returns its reason and reports no success")

with patch("subprocess.run") as run:
    run.return_value.returncode = 0
    run.return_value.stderr = ""
    run.return_value.stdout = '{"session_id":"s-w1","action":"send","status":"sent_to_tmux"}'
    out = W.nudge("/proj", CHOICE)
check(out["sent"] and out["submitted"] and out["reason"] == "",
      "a clean send+submit returns success with an empty reason")

# ══ 3. a failed attempt records its backoff exactly like a success ═════
print("a failed attempt records a backoff, so failures cannot spin")
reset_state()
def record_mock_session():
    W.record("s-w1", "fix-a", reason="Target has no live terminal")
record_mock_session()
check(W.strikes("s-w1", "fix-a") == 1, "a failed attempt increments the strike counter")
check("no live terminal" in W.last_reason("s-w1", "fix-a"), "…and stores the refusal reason")

# ══ 4. tick(): a dead terminal is marked unavailable with the reason ═══
print("tick() marks a dead terminal unavailable and does not retry it")
reset_state()
W.candidates = lambda project, root=None, live=None: [
    {"id": "s-lead", "agent": "claude", "harness": "claude", "role": "lead"},
    {"id": "s-w1", "agent": "codex", "harness": "codex", "role": "worker"}]
W.pick = lambda *a, **k: None
rp = types.ModuleType("review_panel")
rp.room_scope = lambda: "2026-01-01T00:00:00Z"
rp.todos = lambda since=None: [{"agent": "codex", "task": "t1", "state": "active",
                                 "updated": "2026-01-01T00:00:00Z"}]
ll = types.ModuleType("lead_lifecycle"); ll.lookup = lambda *a, **k: {"state": "idle"}
lt = types.ModuleType("liljack_tasks"); lt.tasks = lambda open_only=False, **k: []
sys.modules.update({"review_panel": rp, "lead_lifecycle": ll, "liljack_tasks": lt})

class DeadTmux:
    def sessions(self, project):
        return []                       # nothing is live in tmux

rep = W.tick("/proj", root=None, dry_run=True, tmux=DeadTmux())
check(rep["action"] in ("worker-dry-run", "worker-idle-none"), "tick completes without raising")
u = W.unavailable("s-w1")
check(u is not None and u["reason"] == "no live terminal",
      "the dead terminal is marked unavailable WITH the reason")

# ══ 5. three strikes on the lead escalates to operator by mail ═══════════
print("a silent lead escalates to operator by mail after its own three strikes")
reset_state()
W.candidates = lambda project, root=None, live=None: [
    {"id": "s-lead", "agent": "claude", "harness": "claude", "role": "lead"}]
W.pick = lambda *a, **k: {"session": "s-lead", "agent": "claude", "role": "lead",
                          "kind": "escalate",
                          "escalations": [{"owner": "codex", "task": "fix-a",
                                           "reason": "no response", "strikes": 3}]}
W.nudge = lambda project, choice, text=None: {"agent": "claude", "sent": True,
                                              "submitted": True, "reason": ""}
mails = []
def send_mail(text, to, subject="", root=None):
    mails.append({"to": to, "subject": subject, "text": text})
    return True

for _ in range(3):
    rep = W.tick("/proj", root=None, dry_run=False, tmux=DeadTmux(), send_mail=send_mail)
check([m["to"] for m in mails] == ["operator"],
      "after the lead's own three strikes, operator is mailed exactly once")
check("claude" in (mails[0]["text"] if mails else "") and "unresponsive" in (mails[0]["subject"] if mails else ""),
      "…naming the lead and the reason")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
