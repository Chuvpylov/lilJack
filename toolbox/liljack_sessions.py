#!/usr/bin/env python3
"""
liljack_sessions.py — the live-session registry, N harnesses not 2.

"Who is running right now, on which harness, in which repo, and are they in a
state that can be handed work?" Every answer the coordination layer needs about
a *session* comes from here; `liljack_tasks` owns what the work IS.

⚠ WHY THIS IS A SEPARATE MODULE. The watcher used to answer this question
inline, for exactly one harness, from exactly one sqlite file. Adding a third
agent then means editing the watcher — and this repo has already paid for that
shape three times over ("three copies of `what era is this run?` existed, and
only one knew about vision"). A harness is registered here by writing ONE
adapter and calling `register_source`; nothing in the core changes.

  ADAPTER CONTRACT — a source is a callable returning raw event rows:
      {"agent": str, "harness": str, "session": str, "cwd": str,
       "event": str, "at": ISO-8601 UTC, "seq": int}
  `seq` only has to be monotone WITHIN a session. Everything after that —
  collapsing to one row per session, ageing, the settle delay, the state
  mapping — is done once, here, for every harness at once.

STATES, and what each one means for delivery:
    idle        newest event ends a turn and has settled     → DELIVERABLE
    available   a harness with NO session at all (an API      → callable, but
                model: DeepSeek) that can be called now          NOT deliverable
    settling    a turn end younger than `settle`             → not yet: the
                model may still be streaming its report
    busy        mid-turn                                     → no
    blocked     waiting on a permission prompt               → no, and say so
    compacting  between PreCompact and PostCompact           → no
    ended       SessionEnd — the thread is DEAD              → NEVER

⚠ `ended` is not merely "ranked last". On 2026-09-05 `codex queue` returned
SUCCESS against a thread whose only event was `SessionEnd`; the task was acked
and delivered nowhere. When the failure is silent, ranking is not a guard —
the row has to be dropped.

⚠ `compacting` is a state the old watcher did not have, and its absence was a
real hazard: a `PreCompact` is not a turn end, but nor is the session doing
anything an injected message would survive intact. A compacting session is not
idle and is never a target.

⚠ `SubagentStop` is NOT a turn end — it carries the PARENT session id (see
`liljack_codex.record_event`), so a session whose newest event is SubagentStop
is mid-turn with a helper just finished.

Stdlib only. Every source opens its store READ-ONLY; another process owns it.
"""
from __future__ import annotations

import json
import os
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

CACHE = Path(os.environ.get("LILJACK_CACHE", str(Path.home() / ".cache" / "liljack")))
CODEX_DB = Path(os.environ.get("LILJACK_CODEX_DB",
                               str(Path.home() / ".codex" / "liljack" / "lifecycle.sqlite3")))

SETTLE = 60                 # seconds a turn end must be old before it counts as idle
HORIZON = 24 * 3600         # no event for this long → not a live session at all
EVENT_LIMIT = 400

TURN_END = ("Stop", "SessionEnd", "Interrupt")
BLOCKED_EVENT = "PermissionRequest"
COMPACT_START = "PreCompact"
COMPACT_END = "PostCompact"
END_EVENT = "SessionEnd"
# ⚠ An API model has no turns, so no event of a Codex/Claude session ever maps
# here; a source that emits it is declaring "there is no session, and there does
# not need to be one". `available` is deliberately NOT deliverable: delivery
# means injecting text into a live terminal, and there is none to inject into.
AVAILABLE_EVENT = "Available"

STATES = ("idle", "available", "settling", "busy", "blocked", "compacting", "ended")
DELIVERABLE_STATES = ("idle",)


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def parse_ts(s):
    """ISO-8601 UTC. A bad timestamp is None, never a guess."""
    if not isinstance(s, str) or not s.strip():
        return None
    try:
        dt = datetime.fromisoformat(s.strip().replace("Z", "+00:00"))
    except Exception:
        return None
    return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)


# ══════════════════════════════════════════════════════════════════════════
# the core: raw event rows → one state per session
# ══════════════════════════════════════════════════════════════════════════

def state_of(event: str, age, settle: int = SETTLE) -> str:
    """The state mapping, isolated so it can be tested without a store."""
    if event == END_EVENT:
        return "ended"
    if event == BLOCKED_EVENT:
        return "blocked"
    if event == COMPACT_START:
        return "compacting"
    if event == AVAILABLE_EVENT:
        return "available"
    if event in TURN_END:
        return "idle" if (age is not None and age >= settle) else "settling"
    return "busy"


def collapse(rows, agent: str = "", harness: str = "", now=None, settle: int = SETTLE,
             horizon: int = HORIZON) -> list:
    """Newest event per session → one state row per session, newest first.

    Sessions whose newest event is older than `horizon` are DROPPED: a thread
    nobody has touched for a day is not a live target, whatever its last event
    said.
    """
    now = now or utcnow()
    by = {}
    for r in rows or []:
        sid = r.get("session")
        if not sid:
            continue
        seq = r.get("seq") or 0
        cur = by.get(sid)
        if cur is None or seq > (cur.get("seq") or 0):
            by[sid] = dict(r, seq=seq)
    out = []
    for s in by.values():
        at = parse_ts(s.get("at"))
        age = (now - at).total_seconds() if at else None
        if age is not None and age > horizon:
            continue
        event = s.get("event") or ""
        out.append({"session": s["session"], "seq": s.get("seq") or 0,
                    "cwd": s.get("cwd") or "", "event": event, "at": s.get("at") or "",
                    "agent": (s.get("agent") or agent or "").lower(),
                    "harness": (s.get("harness") or harness or s.get("agent") or "").lower(),
                    "age": age, "state": state_of(event, age, settle),
                    "ended": event == END_EVENT})
    return sorted(out, key=lambda s: s["seq"], reverse=True)


def pick_target(states, cwd_filter=None, agent: str = ""):
    """The one session to hand work to, or None.

    Deliverable means: state `idle`, and NOT ended. Both conditions are checked
    — legacy callers hand in rows that say `state="idle", ended=True` for a
    SessionEnd, and that row must still be refused.
    """
    cands = [s for s in states
             if s.get("state") in DELIVERABLE_STATES and not s.get("ended")]
    if agent:
        want = agent.strip().lower()
        cands = [s for s in cands if (s.get("agent") or want) == want]
    if cwd_filter:
        want_cwd = str(Path(cwd_filter).resolve())
        cands = [s for s in cands if s.get("cwd") and str(Path(s["cwd"])) == want_cwd]
    if not cands:
        return None
    return sorted(cands, key=lambda s: -(s.get("seq") or 0))[0]


# ══════════════════════════════════════════════════════════════════════════
# sources — one adapter per harness; the registry is a plain dict
# ══════════════════════════════════════════════════════════════════════════

SOURCES = {}


def register_source(name: str, fn) -> None:
    """Add a harness. `fn(**ctx) -> {"ok": bool, "rows": [...], "error": str}`.

    That is the ENTIRE extension point. A third harness is one function and one
    call to this; no state mapping, no target selection and no caller changes.
    """
    SOURCES[str(name).strip().lower()] = fn


def codex_source(db_path=None, limit: int = EVENT_LIMIT, rows=None, **_) -> dict:
    """Codex's own lifecycle DB. ⚠ Opened `mode=ro` through a URI and never
    written — another process owns it. `rows=` injects a fixture in tests."""
    if rows is not None:
        return {"ok": True, "rows": [dict(r, agent="codex", harness="codex") for r in rows]}
    p = Path(db_path or CODEX_DB)
    if not p.exists():
        return {"ok": False, "error": f"no lifecycle db at {p}", "rows": []}
    try:
        con = sqlite3.connect(f"file:{p}?mode=ro", uri=True, timeout=2.0)
        try:
            cur = con.execute("select seq, recorded_at, session_id, cwd, event "
                              "from events order by seq desc limit ?", (int(limit),))
            got = [{"seq": r[0], "at": r[1], "session": r[2], "cwd": r[3], "event": r[4],
                    "agent": "codex", "harness": "codex"} for r in cur.fetchall()]
        finally:
            con.close()
    except Exception as exc:
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}", "rows": []}
    return {"ok": True, "rows": got, "path": str(p)}


def jsonl_source(root=None, harness: str = "", **_) -> dict:
    """The generic adapter: `<root>/sessions/<harness>.jsonl`, one event per line.

    This is the demonstration that the extension point is real — a harness with
    no sqlite of its own (a shell wrapper, a future agent, Claude Code itself
    once its Stop hook writes here) becomes visible by appending lines in the
    same shape the sqlite one produces. It also means the registry is testable
    with N agents without inventing N databases.
    """
    d = (Path(root) if root else CACHE) / "sessions"
    if not d.is_dir():
        return {"ok": False, "error": f"no session dir at {d}", "rows": []}
    want = (harness or "").strip().lower()
    out, seq = [], 0
    for f in sorted(d.glob("*.jsonl")):
        who = f.stem.lower()
        if want and who != want:
            continue
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except Exception:
                continue                       # a torn line is skipped, never fatal
            if not isinstance(r, dict) or not r.get("session"):
                continue
            seq += 1
            out.append({"seq": r.get("seq") or seq, "at": r.get("at") or "",
                        "session": r["session"], "cwd": r.get("cwd") or "",
                        "event": r.get("event") or "",
                        "agent": (r.get("agent") or who), "harness": who})
    return {"ok": True, "rows": out, "path": str(d)}


register_source("codex", codex_source)
register_source("jsonl", jsonl_source)


# ⚠ AUTOLOAD IS GENERIC AND OPT-IN. A harness that lives in its own module
# (`liljack_deepseek`) has to get its `register_source` call made by SOMEBODY,
# and the one thing that must not happen is this file importing it by name —
# that would put the third agent back in the core and undo the extension point
# the module header argues for. So: `LILJACK_SESSION_SOURCES=liljack_deepseek,…`
# names modules to import; each supplies `register(sessions)`. Unset (the
# default) is a no-op, so nothing changes for anyone who has not asked.
AUTOLOAD_ENV = "LILJACK_SESSION_SOURCES"
_autoloaded = {}


def autoload(spec=None) -> dict:
    """Import the modules named in the env and let each register itself.

    Returns {module: error} for the ones that failed. Idempotent: a module is
    attempted once per process, so `snapshot` may call it on every tick.
    """
    names = [n.strip() for n in
             (spec if spec is not None else os.environ.get(AUTOLOAD_ENV, ""))
             .replace(";", ",").split(",") if n.strip()]
    errs = {}
    for name in names:
        if name in _autoloaded:
            if _autoloaded[name]:
                errs[name] = _autoloaded[name]
            continue
        try:
            mod = __import__(name)
            mod.register(sys.modules[__name__])
            _autoloaded[name] = ""
        except Exception as exc:
            _autoloaded[name] = f"{type(exc).__name__}: {exc}"
            errs[name] = _autoloaded[name]
    return errs


def snapshot(sources=None, now=None, settle: int = SETTLE, horizon: int = HORIZON,
             **ctx) -> dict:
    """Every live session every registered source can see.

    Returns {"sessions": [...], "errors": {source: message}}. A source that
    cannot answer is REPORTED, never silently absent — a missing harness and an
    idle harness are different facts and the watcher acts differently on them.
    """
    errors = dict(autoload())
    names = list(sources) if sources else list(SOURCES)
    out = []
    for name in names:
        fn = SOURCES.get(name)
        if fn is None:
            errors[name] = "no such source"
            continue
        try:
            res = fn(**ctx)
        except Exception as exc:
            errors[name] = f"{type(exc).__name__}: {exc}"
            continue
        if not res.get("ok"):
            errors[name] = res.get("error", "unavailable")
            continue
        out += collapse(res.get("rows") or [], harness=name, now=now,
                        settle=settle, horizon=horizon)
    return {"sessions": sorted(out, key=lambda s: -(s.get("seq") or 0)), "errors": errors}


def agents_live(snap: dict) -> dict:
    """agent → its best state, for a digest line. `idle` beats `busy` beats the
    rest; a session that only ever `ended` reports `ended`, not silence."""
    rank = {"idle": 0, "available": 0, "settling": 1, "busy": 2, "compacting": 3,
            "blocked": 4, "ended": 5}
    best = {}
    for s in snap.get("sessions", []):
        a = s.get("agent") or s.get("harness") or "?"
        if a not in best or rank.get(s["state"], 9) < rank.get(best[a]["state"], 9):
            best[a] = s
    return best


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="lilJack live-session registry")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--settle", type=int, default=SETTLE)
    ap.add_argument("--source", action="append", default=[])
    a = ap.parse_args(argv)
    snap = snapshot(sources=a.source or None, settle=a.settle)
    if a.json:
        print(json.dumps(snap, indent=2, ensure_ascii=False))
        return 0
    for name, err in snap["errors"].items():
        print(f"✗ {name}: {err}")
    for s in snap["sessions"]:
        age = f"{int(s['age'])}s" if s["age"] is not None else "?"
        print(f"{s['state']:<10} {s['agent'] or s['harness']:<8} {s['session'][:12]:<13} "
              f"{s['event']:<18} {age:>7}  {s['cwd']}")
    if not snap["sessions"]:
        print("no live sessions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
