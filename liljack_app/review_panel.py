"""Data for the App review panel: session TODOs, completion, git history, fit.

The panel is a VIEW over things that already exist — the BMD agent board, git,
and the session scores — so nothing here is a second source of truth. It only
reads, and it is deliberately dull: gathering happens off the draw path and
every getter returns a plain structure the renderer can lay out.

⚠ Missing data is REPORTED, never faked. A board that cannot be read, a repo
that is not a repo, scores that were never recorded — each returns an explicit
absence with a reason. A panel that quietly shows zeros is worse than one that
says it does not know, because zeros look like progress.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

__all__ = ["summary", "todos", "git_history", "agent_fit", "attention", "open_work",
           "room_scope", "room_log", "git_dag", "snapshot", "MARKS", "ORDER", "STALE_AFTER_S"]

MARKS = {"done": "✓", "active": "▶", "queued": "○", "blocked": "⛔"}
ORDER = {"blocked": 0, "active": 1, "queued": 2, "done": 3}
AGENTS = ("claude", "codex", "deepseek")
# A notice older than this is history, not a request. Delivering it wakes the
# lead to answer something already answered, which is how a queue turns into
# noise and the room loses its focus.
STALE_AFTER_S = 15 * 60
REPO = Path(__file__).resolve().parents[1]


def _board():
    try:
        import sys
        sys.path.insert(0, str(REPO / "toolbox"))
        import liljack_board
        return liljack_board.read(), None
    except Exception as exc:                      # never break the UI
        return None, f"{type(exc).__name__}: {exc}"


def room_scope(root=None):
    """When this project's room began — the honest boundary of "our work".

    The board is the WHOLE project: it carries trio judging, fpga_lab and other
    standing tasks that this room never planned. Counting those makes a room's
    progress meaningless and would keep a "stop when done" rule alive forever.
    Returns an ISO timestamp, or None when there is no room to scope to.
    """
    import sqlite3
    base = Path(root or os.environ.get("LILJACK_WORKSPACE_ROOT",
                                       str(Path.home() / ".cache/liljack/workspace")))
    db = base / "workspace.sqlite3"
    if not db.exists():
        return None
    try:
        con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        row = con.execute("SELECT created_at FROM events WHERE kind='room' ORDER BY seq LIMIT 1").fetchone()
        con.close()
    except sqlite3.Error:
        return None
    return row[0] if row else None


def _items(day=None, since=None):
    b, err = _board()
    if b is None:
        return [], err
    rows = [i for i in b.get("items", []) if i.get("agent") in AGENTS]
    if day:
        rows = [i for i in rows if str(i.get("updated", "")).startswith(day)]
    if since:
        rows = [i for i in rows if str(i.get("updated", "")) >= since]
    return rows, None


def summary(day=None, since=None) -> dict:
    """Completion for the session, overall and per agent."""
    rows, err = _items(day, since)
    if err:
        return {"error": err, "total": 0}
    done = sum(1 for i in rows if i["state"] == "done")
    per = {}
    for a in AGENTS:
        mine = [i for i in rows if i["agent"] == a]
        d = sum(1 for i in mine if i["state"] == "done")
        per[a] = {"done": d, "total": len(mine),
                  "pct": (100 * d // len(mine)) if mine else 0}
    return {"total": len(rows), "done": done,
            "pct": (100 * done // len(rows)) if rows else 0,
            "active": sum(1 for i in rows if i["state"] == "active"),
            "queued": sum(1 for i in rows if i["state"] == "queued"),
            "blocked": sum(1 for i in rows if i["state"] == "blocked"),
            "per_agent": per, "error": None}


def todos(day=None, since=None, limit=200) -> list:
    """Rows ready to draw: blocked first, then active, queued, done."""
    rows, err = _items(day, since)
    if err:
        return []
    rows.sort(key=lambda i: (ORDER.get(i["state"], 9), i["agent"], i["task"]))
    return [{"agent": i["agent"], "task": i["task"], "state": i["state"],
             "mark": MARKS.get(i["state"], "?"), "progress": i.get("progress", ""),
             "updated": i.get("updated", "")}
            for i in rows[:limit]]


def git_history(limit=20, repo=None) -> dict:
    """Recent commits with their graph column, straight from git."""
    root = Path(repo or REPO)
    try:
        r = subprocess.run(
            ["git", "-C", str(root), "log", "--graph", "--date=short",
             f"--max-count={int(limit)}", "--pretty=format:%h\x1f%ad\x1f%s"],
            capture_output=True, stdin=subprocess.DEVNULL, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as exc:
        return {"rows": [], "error": f"{type(exc).__name__}: {exc}"}
    if r.returncode != 0:
        return {"rows": [], "error": (r.stderr or "git failed").strip()[:200]}
    out = []
    for line in r.stdout.splitlines():
        graph, sep, rest = line.partition("\x1f")
        if not sep:                                # a pure graph line (merge rails)
            out.append({"graph": line.rstrip(), "sha": "", "date": "", "subject": ""})
            continue
        sha = graph[graph.rfind(" ") + 1:] if " " in graph else graph
        date, _, subject = rest.partition("\x1f")
        out.append({"graph": graph[:graph.rfind(" ") + 1] if " " in graph else "",
                    "sha": sha, "date": date, "subject": subject})
    return {"rows": out, "error": None}


def agent_fit() -> dict:
    """Per-agent scores, for routing a task to whoever actually does it well.

    ⚠ Returns an explicit absence when scores were never recorded. A default of
    'everyone is equally good' would silently become the routing policy.
    """
    try:
        import sys
        sys.path.insert(0, str(REPO / "toolbox"))
        import liljack_scores
        s = liljack_scores.Scores()
        try:
            fam = s.family_scores() or {}
        finally:
            s.close()
    except Exception as exc:
        return {"scores": {}, "error": f"{type(exc).__name__}: {exc}"}
    if not fam:
        return {"scores": {}, "error": "no scores recorded yet"}
    # ⚠ Name EVERY agent, including the ones with no rating. Listing only the
    # rated ones makes an absent agent invisible rather than unrated, and an
    # invisible agent silently drops out of any routing decision.
    out = {}
    for a in AGENTS:
        v = fam.get(a)
        out[a] = dict(v) if isinstance(v, dict) else {"score": None, "sessions": 0,
                                                      "reason": "never self-rated"}
    return {"scores": out, "error": None}


def attention(root=None, now=None) -> dict:
    """What the lead still has to look at, and what is merely old.

    Splits UNDELIVERED room notices by age. Fresh ones are the focus.

    ⚠ An old pending notice is NOT a handled one. DeepSeek caught this: these
    rows are state='pending', meaning never delivered, so age alone cannot say
    anyone dealt with them — an old one may be old precisely BECAUSE nobody saw
    it. The count is reported as "older, still undelivered", which is what the
    data supports, and delivery state is carried so a caller cannot re-invent
    the claim.
    """
    import sqlite3
    import time as _t
    from datetime import datetime
    base = Path(root or os.environ.get("LILJACK_WORKSPACE_ROOT",
                                       str(Path.home() / ".cache/liljack/workspace")))
    db = base / "workspace.sqlite3"
    if not db.exists():
        return {"fresh": [], "stale": 0, "oldest_stale_min": 0,
                "error": f"no workspace store at {db}"}
    now = now or _t.time()
    fresh, stale, oldest = [], 0, 0.0
    try:
        con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        rows = con.execute(
            "SELECT m.sender, m.created_at, m.text FROM room_delivery d "
            "JOIN messages m ON m.id = d.message_id WHERE d.state='pending' ORDER BY m.seq").fetchall()
        con.close()
    except sqlite3.Error as exc:
        return {"fresh": [], "stale": 0, "oldest_stale_min": 0, "error": str(exc)}
    for sender, created, text in rows:
        try:
            age = now - datetime.fromisoformat(str(created).replace("Z", "+00:00")).timestamp()
        except (ValueError, TypeError):
            age = 0.0
        if age > STALE_AFTER_S:
            stale += 1
            oldest = max(oldest, age)
        else:
            fresh.append({"sender": sender, "age_min": int(age // 60),
                          "text": (text or "")[:120]})
    return {"fresh": fresh, "stale": stale, "stale_state": "undelivered",
            "oldest_stale_min": int(oldest // 60), "error": None}


def git_dag(repo=None, limit=40) -> dict:
    """Commit topology with LANES assigned, ready to draw.

    graph_data.snapshot() (Codex) supplies nodes and parents; the lane
    assignment is the drawing half and lives here so the C renderer only has to
    put marks on a grid.

    Lanes follow the usual rule: a commit inherits the lane of the first
    still-open child that expects it, and each parent then reserves a lane. A
    merge therefore keeps its first parent in place and pushes the second out,
    which is what makes a branch read as a branch instead of a zigzag.
    """
    try:
        from graph_data import snapshot as _snap
    except Exception:
        try:
            from .graph_data import snapshot as _snap
        except Exception as exc:
            return {"rows": [], "lanes": 0, "truncated": False,
                    "error": f"topology unavailable: {type(exc).__name__}"}
    raw = _snap(repo or REPO, limit)
    if raw.get("error"):
        return {"rows": [], "lanes": 0, "truncated": raw.get("truncated", False),
                "error": raw["error"]}
    lanes: list = []            # lane -> commit id it is waiting for ("" = free)
    rows = []
    for node in raw["nodes"]:
        cid = node["id"]
        try:
            lane = lanes.index(cid)
        except ValueError:
            lane = next((i for i, v in enumerate(lanes) if not v), len(lanes))
            if lane == len(lanes):
                lanes.append("")
        lanes[lane] = ""
        parents = node.get("parents") or []
        if parents:
            lanes[lane] = parents[0]
        for extra in parents[1:]:      # a merge opens a new rail for each extra parent
            if extra in lanes:
                continue
            free = next((i for i, v in enumerate(lanes) if not v), len(lanes))
            if free == len(lanes):
                lanes.append(extra)
            else:
                lanes[free] = extra
        rows.append({"id": cid, "short": cid[:7], "lane": lane,
                     "open": [i for i, v in enumerate(lanes) if v],
                     "merge": len(parents) > 1,
                     "refs": node.get("refs", ""), "date": (node.get("date") or "")[:10],
                     "subject": node.get("subject", "")})
    return {"rows": rows, "lanes": max((len(r["open"]) for r in rows), default=0) + 1,
            "truncated": raw.get("truncated", False), "error": None}


def room_log(limit=12, root=None) -> dict:
    """The room conversation itself, newest last — the thread, not the queue.

    ATTENTION answers "what still needs me"; this answers "what was said". They
    are different questions and conflating them is how a room loses its history
    the moment a notice is marked read.
    """
    import sqlite3
    base = Path(root or os.environ.get("LILJACK_WORKSPACE_ROOT",
                                       str(Path.home() / ".cache/liljack/workspace")))
    db = base / "workspace.sqlite3"
    if not db.exists():
        return {"rows": [], "error": f"no workspace store at {db}"}
    try:
        con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        rows = con.execute(
            "SELECT sender, created_at, text FROM messages ORDER BY seq DESC LIMIT ?",
            (int(limit),)).fetchall()
        con.close()
    except sqlite3.Error as exc:
        return {"rows": [], "error": str(exc)}
    out = []
    for sender, created, text in reversed(rows):
        first = (text or "").strip().split("\n", 1)[0]
        out.append({"sender": str(sender), "at": str(created)[11:16],
                    "text": first[:150]})
    return {"rows": out, "error": None}


def open_work(day=None, since="room") -> dict:
    """How much ACTIONABLE work is still open, for the heartbeat's own stop rule.

    ⚠ BLOCKED IS NOT ACTIONABLE. A blocked todo is present but nobody can act on
    it right now, so it must not keep a heartbeat alive (see liljack_tasks.PENDING)
    — a room whose work is all blocked would otherwise tick forever with nothing
    to deliver. Blocked still surfaces in `summary()` (its own count) and in the
    retire reason; this function reports only active+queued, which is what the
    loop can actually drive.

    ⚠ UNKNOWN IS NEVER DONE. If the board cannot be read this returns an error
    and NO count, so a caller must not read a failure as "nothing left". The two
    mistakes are not equal: stopping early strands the team with no way to be
    woken, while staying up one cycle too long costs a few local sqlite reads.
    """
    if since == "room":
        since = room_scope()
    rows, err = _items(day, since)
    if err:
        return {"open": None, "error": err, "since": since}
    return {"open": sum(1 for i in rows if i["state"] in ("active", "queued")),
            "error": None, "since": since}


def snapshot(day=None, repo=None) -> dict:
    """Everything the panel draws, gathered once, off the draw path.

    `repo` selects which working tree the git history describes, so the panel
    follows the SELECTED project rather than always reporting demo.
    """
    scope = room_scope()
    return {"summary": summary(day, scope), "todos": todos(day, scope),
            "git": git_history(repo=repo), "fit": agent_fit(),
            "attention": attention(), "room_log": room_log(),
            "git_dag": git_dag(repo=repo)}
