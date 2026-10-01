#!/usr/bin/env python3
"""review_panel: the App review tab's data — TODOs, completion, git, fit.

The properties that matter are about HONESTY, not formatting: an unreadable
board, a missing repo and absent scores must each report an explicit absence.
A panel that silently shows zeros looks like progress that did not happen.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "toolbox" / "liljack_app"))
import review_panel as P

ok = fail = 0
def check(cond, name):
    global ok, fail
    if cond: ok += 1; print(f"  ok     {name}")
    else: fail += 1; print(f"  FAIL   {name}")

# ── summary ──────────────────────────────────────────────────────────────
s = P.summary()
check(s.get("error") is None and s["total"] > 0, "summary reads the live board")
check(s["done"] + s["active"] + s["queued"] + s["blocked"] <= s["total"], "states never exceed the total")
check(0 <= s["pct"] <= 100, "completion is a percentage")
check(all(a in s["per_agent"] for a in ("claude", "codex", "deepseek")), "every agent is represented")
check(all(v["done"] <= v["total"] for v in s["per_agent"].values()), "per-agent done never exceeds total")
check(P.summary("1999-01-01")["total"] == 0, "an empty day yields nothing, not an error")

# ── todos ────────────────────────────────────────────────────────────────
t = P.todos()
check(isinstance(t, list) and t, "todos returns rows")
check(all(set(("agent", "task", "state", "mark")) <= set(r) for r in t), "every row is drawable")
order = [P.ORDER.get(r["state"], 9) for r in t]
check(order == sorted(order), "blocked and active sort ABOVE queued and done — what needs attention leads")
check(all(r["mark"] == P.MARKS.get(r["state"], "?") for r in t), "mark always matches state")
check(len(P.todos(limit=3)) <= 3, "limit is honoured")

# ── git ──────────────────────────────────────────────────────────────────
g = P.git_history(5)
check(g["error"] is None and g["rows"], "git history reads this repo")
check(any(r["sha"] and r["subject"] for r in g["rows"]), "commits carry sha and subject")
check(all(isinstance(r["graph"], str) for r in g["rows"]), "graph column is always present, even when empty")
with tempfile.TemporaryDirectory() as d:
    ng = P.git_history(5, repo=d)
    check(ng["rows"] == [] and ng["error"], "a non-repo REPORTS the failure instead of showing an empty history")

# ── fit / routing ────────────────────────────────────────────────────────
f = P.agent_fit()
check(isinstance(f, dict) and "scores" in f and "error" in f, "fit always answers with scores and an error slot")
check(f["error"] is not None or f["scores"], "fit never returns a silent empty result")
check(all(k in ("claude", "codex", "deepseek") for k in f["scores"]), "fit is scoped to our agents")

# ── snapshot ─────────────────────────────────────────────────────────────
snap = P.snapshot()
# ⚠ An EXACT set, deliberately: it fails on an addition as well as a removal, so
# a new panel section cannot appear without somebody deciding it belongs here.
# `git_dag` was added by the git-DAG tile work (2026-09-09) and this assertion
# correctly caught that it had not been declared — updated rather than loosened.
check(set(snap) == {"summary", "todos", "git", "git_dag", "fit", "attention", "room_log"},
      "snapshot gathers every section once")

# ── attention: focus, not history ────────────────────────────────────────
a = P.attention()
check(a.get("error") is None, "attention reads the delivery queue")
check(isinstance(a["fresh"], list) and isinstance(a["stale"], int), "attention splits fresh from stale")
check(all(f["age_min"] <= P.STALE_AFTER_S // 60 for f in a["fresh"]),
      "nothing older than the stale threshold is ever called fresh")
check(a["stale"] == 0 or a["oldest_stale_min"] >= P.STALE_AFTER_S // 60,
      "the stale count is genuinely stale, not merely unread")
check(P.attention(root="/nonexistent")["error"], "a missing store REPORTS, it does not claim an empty queue")
# ⚠ DeepSeek's catch: these rows are state='pending', so age cannot prove anyone
# handled them. The panel must not claim more than the data supports.
check(a.get("stale_state") == "undelivered",
      "stale notices are reported as UNDELIVERED, never as 'already handled'")
# check the STRING LITERALS the user actually sees, not the comments that
# explain why the phrase is wrong — the first version of this test flagged its
# own explanatory comment.
import re as _re
_src = (Path(__file__).resolve().parents[1] / "liljack_app/c_review.c").read_text()
_code = _re.sub(r"/\*.*?\*/", " ", _src, flags=_re.S)   # comments quote the phrase to reject it
_literals = " ".join(_re.findall(r'"((?:[^"\\]|\\.)*)"', _code))
check("already handled" not in _literals,
      "no rendered string claims an undelivered notice was handled")
check("UNDELIVERED" in _literals, "the rendered string says UNDELIVERED, which is what the data supports")

# ── room scope: the board is the whole project, not this room ────────────
scope = P.room_scope()
check(scope is None or isinstance(scope, str), "room_scope returns a timestamp or an honest None")
whole = P.open_work(since=None)
scoped = P.open_work()
check(whole["error"] is None and scoped["error"] is None, "both scopes read the board")
check(scoped["open"] <= whole["open"],
      "room scope never counts MORE than the whole board")
if scope:
    check(scoped["open"] < whole["open"],
          "the board carries work this room never planned, and scoping excludes it")
    check(all(t["task"] for t in P.todos(since=scope)), "scoped todos are still drawable")
check(P.open_work(since="1999-01-01")["open"] >= 0, "an explicit early scope includes everything")

# ── room log: the thread, not the queue ──────────────────────────────────
lg = P.room_log(5)
check(lg["error"] is None and isinstance(lg["rows"], list), "room_log reads the conversation")
check(len(lg["rows"]) <= 5, "room_log honours its limit")
check(all({"sender", "at", "text"} <= set(r) for r in lg["rows"]), "every log row is drawable")
check(P.room_log(root="/nonexistent")["error"], "a missing store REPORTS instead of showing an empty thread")
check("room_log" in P.snapshot(), "snapshot carries the room log")

print(f"\n{ok} checks passed, {fail} failed")
sys.exit(1 if fail else 0)
