#!/usr/bin/env python3
"""dashboard-backend-host-and-snapshot — the home screen's data.

the operator via the the dashboard plan: "who is working, on what, how far along, on
which host". Rules planted here:
  1. every session observation carries a `host` (this node), and an imported
     row keeps the host it came with;
  2. the dashboard lists only LIVE agents, never stopped/exited ones;
  3. an unavailable agent carries its reason, so the UI shows "—" not a zero;
  4. work rows come from the registry rows the snapshot already has, with a
     done/open split, and each says whether its owner session is live;
  5. hosts are deduplicated and sorted; the local node is reported separately;
  6. git comes from the review panel and stays None/'' when absent — never 0.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(ROOT))

import platform  # noqa: E402
import liljack_workspace as W  # noqa: E402
from liljack_app.backend import Backend  # noqa: E402

ok = fail = 0
def check(c, n):
    global ok, fail
    if c: ok += 1; print(f"  ok     {n}")
    else: fail += 1; print(f"  FAIL   {n}")

row = {"harness": "tmux", "session": "n1", "cwd": "/tmp/p", "agent": "codex",
       "state": "running", "at": "2026-09-12T08:00:00+00:00"}
obs = W.Workspace._observation(row)
check(obs["host"] == (platform.node() or ""), "an observation carries this node's host")
imported = W.Workspace._observation({**row, "host": "other-box"})
check(imported["host"] == "other-box", "an imported row keeps the host it came with")

b = Backend.__new__(Backend)
snap = {
    "sessions": [
        {"id": "s-live", "agent": "codex", "state": "running", "host": "devbox",
         "task": "build the gallery", "role": "worker"},
        {"id": "s-lead", "agent": "deepseek", "state": "running", "host": "devbox", "role": "lead"},
        {"id": "s-far", "agent": "claude", "state": "running", "host": "vps-1"},
        {"id": "s-gone", "agent": "claude", "state": "stopped", "host": "devbox"},
        {"id": "s-dup", "agent": "codex", "state": "running", "host": "devbox"},
        {"id": "s-dup", "agent": "codex", "state": "running", "host": "devbox"},
        {"id": "s-sad", "agent": "codex", "state": "running", "host": "devbox",
         "unavailable": {"reason": "no credit", "at": "2026-09-12T07:00:00+00:00"}},
    ],
    "tasks": [
        {"task": "t-a", "agent": "codex", "state": "active", "owner_session": "s-live", "progress": "3/7"},
        {"task": "t-b", "agent": "claude", "state": "done", "owner_session": "s-far"},
        {"task": "t-c", "agent": "deepseek", "state": "blocked", "owner_session": "s-ghost"},
    ],
    "review_panel": {"git": {"branch": "master", "rows": [1, 2, 3, 4]}, "error": ""},
}
d = b.dashboard(snap)

ids = [a["session"] for a in d["agents"]]
check("s-gone" not in ids, "a stopped session is not on the dashboard")
check(ids.count("s-dup") == 1, "a duplicated session row is listed once")
check(len(ids) == 5, f"five live agents listed (got {len(ids)}: {ids})")
sad = next(a for a in d["agents"] if a["session"] == "s-sad")
check(sad["unavailable"] == "no credit", "an unavailable agent carries its reason")
live = next(a for a in d["agents"] if a["session"] == "s-live")
check(live["task"] == "build the gallery" and live["role"] == "worker" and live["host"] == "devbox",
      "an agent card carries task, role and host")
check(next(a for a in d["agents"] if a["session"] == "s-lead")["role"] == "lead",
      "the lead's role is carried through")

check(d["totals"] == {"agents": 5, "tasks": 3, "done": 1, "open": 2},
      f"totals split done/open (got {d['totals']})")
work = {w["task"]: w for w in d["work"]}
check(work["t-a"]["progress"] == "3/7" and work["t-a"]["known_agent"],
      "a work row carries progress and knows its owner session is live")
check(not work["t-c"]["known_agent"],
      "a work row whose owner session is gone says so, rather than guessing")

check(d["hosts"] == ["devbox", "vps-1"], f"hosts deduplicated and sorted (got {d['hosts']})")
check(d["host"] == (platform.node() or ""), "the local node is reported separately")
check(d["git"]["branch"] == "master" and d["git"]["rows"] == 4 and d["git"]["commits"] is None,
      "git carries branch and row count; an absent count stays None, never 0")

empty = b.dashboard({})
check(empty["agents"] == [] and empty["work"] == []
      and empty["totals"] == {"agents": 0, "tasks": 0, "done": 0, "open": 0}
      and empty["git"]["branch"] == "" and empty["git"]["rows"] is None,
      "an empty snapshot yields an empty dashboard, not a fabricated one")

print(f"\n{ok} passed, {fail} failed")
sys.exit(1 if fail else 0)
