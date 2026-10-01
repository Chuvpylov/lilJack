#!/usr/bin/env python3
"""
liljack_tui.py — lilJack's own terminal, a HUI-flavoured curses app.

the operator, 2026-09-05: *"liljack's own interface should draw that — a separate
terminal app with full formatting like claude / opencode harness, we already
had something similar."* The something similar is `arena/arena_browser.c`: a
tab bar across the top, bordered panels, dense rows, one status ribbon at the
bottom. Same layout language, Python/curses instead of HUI/C.

Four tabs:

  BOARD   the Claude ↔ Codex ↔ the operator message board (toolbox/liljack_mail.py),
          grouped into threads, unread marked, compose/reply/ack from here,
          filtered by project so a repo's traffic is one conversation.
  STATUS  what this machine is actually doing: live trainer + step/loss/tok_s,
          the Den's Oracorker timers and walks, judge coverage per shard,
          the TODO line, host load and package temperature.
  PEERS   who else is working right now — every live `claude` and `codex`
          process with the repo it is sitting in — plus lilJack's mood, the
          Codex lifecycle stream and each agent's mail counters.
  RUNS    the tail of runs/atlas.jsonl (the append-only record) and the walk
          state, so a finished run is visible without leaving the terminal.
  CHAT    the three agents (claude · codex · deepseek) as tiles — transcript
          tails, state, unread — with a CONTROL banner saying who holds the
          floor, an input line into the focused tile, take/give control.
          Everything comes from the agent daemon's socket (liljack_chat.py);
          `--demo` runs it against an in-process fake, nothing is spawned.

── coordinating several sessions on one repo ──────────────────────────────

the operator, 2026-09-05: *"i can run multiple codex and claude code sessions so they
need to know who is working on a same repo and cooperate there, not scream to
all chats."*

The mailbox addresses AGENTS (`claude` / `codex` / `operator` / `all`), not
sessions — so mail to `claude` reaches every Claude session, and `all` reaches
everyone. This app does not pretend otherwise; it makes the coordination
visible instead:

  · PEERS reads `/proc/<pid>/cwd` for every live `claude` and `codex` process
    and groups them by git repo. Two sessions in one repo is flagged, because
    that is the case where uncoordinated edits collide.
  · The mailbox's `project` field is the coordination key that actually
    narrows an audience, so compose defaults it to the repo you are in, and
    BOARD filters threads by project (`p` cycles). A repo's traffic is then
    one conversation instead of a shared shout.
  · A message `to: all` is drawn with a 📢 marker and compose warns before
    sending one — broadcasting is a deliberate act, not the default.

── the two rules this file is built around ────────────────────────────────

⚠ **Every number on screen is read at draw time from a named source, and a
missing source says so.** This repo has been bitten repeatedly by status
displays that lied (a stale "◀ CURRENT" marker, an Atlas plotting last-val as
best-val, three disagreeing copies of "which run is live"). So: no defaults,
no last-known-good, no interpolation. If the Den is down the panel prints
`den 7700 unreachable` and the reason; if no trainer holds the lock it prints
`no trainer`. Each panel header carries its own provenance in dim text.

⚠ **Read-only, except the mailbox.** The only writes this program performs are
`liljack_mail.send()` and `liljack_mail.ack()` — both append one line through
the mailbox API, which is what makes it safe beside a concurrently-writing
harness. Nothing here writes into `the trainer/runs/`, `the trainer/data/`, the
judge store or a systemd unit, and nothing starts or stops anything. The
Codex lifecycle DB is opened `mode=ro` because it is another live process's
database. `CUDA_VISIBLE_DEVICES=""` is set at import: no view needs the card.

Stdlib only (`curses` is stdlib) — the rest of toolbox/ is stdlib-only because
the Den imports it, and this file follows the same rule so it can never be the
thing that breaks a Den restart.

Rendering is separated from curses on purpose: every view is a function
`snapshot -> [(style, text)]`, so `tests/test_liljack_tui.py` can check the
formatting with planted inputs and no terminal at all. `--once` prints those
same lines to stdout.

Usage:
    python3 toolbox/liljack_tui.py            # the app
    python3 toolbox/liljack_tui.py --once     # one plain-text dump, no curses
    python3 toolbox/liljack_tui.py --agent operator --tab status
"""
from __future__ import annotations

import json
import os
import re
import sqlite3
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

os.environ.setdefault("CUDA_VISIBLE_DEVICES", "")   # no view needs the card

HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent
BRAIN = PROJECT_ROOT.parent
SIBLING = BRAIN / "the trainer"

if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import liljack_mail as mail                                    # noqa: E402
import liljack_chat as chat                                    # noqa: E402

RING_HEADER = os.environ.get("LILJACK_ARCHIVE_AUTH_HEADER", "X-Ring-Token")  # auth header of the remote archive

DEN_URL = os.environ.get("LILJACK_DEN_URL", "http://localhost:7700")
CODEX_DB = Path(os.environ.get("LILJACK_CODEX_DB",
                               str(Path.home() / ".codex" / "liljack" / "lifecycle.sqlite3")))
TRAIN_RE = re.compile(r"train(_vision)?\.py\s+--config")
CONFIG_RE = re.compile(r"--config\s+(\S+)")
TOTAL_STEPS_RE = re.compile(r"^\s*total_steps\s*:\s*([0-9_]+)", re.M)
JUDGE_KINDS = ("block", "record", "school")

# argv[0] basenames that ARE an interactive agent session. Exact match, because
# the helpers sit right next to them: `codex-code-mode-host` is a code-mode
# sidecar and `claude --chrome-native-host` is the browser bridge — neither is
# somebody working on a repo, and counting them would invent a peer.
AGENT_EXE = {"claude": "claude", "codex": "codex"}
NOT_A_SESSION = ("--chrome-native-host", "--mcp-server", "mcp serve")

TABS = ("BOARD", "STATUS", "PEERS", "RUNS", "CHAT")
ALL_PROJECTS = "(all)"

# ══════════════════════════════════════════════════════════════════════════
# pure helpers — no I/O, no curses, all covered by tests
# ══════════════════════════════════════════════════════════════════════════


def fit(s, w, ell="…"):
    """Clip to w columns. Returns "" for w<=0 so a 1-column terminal cannot raise."""
    s = "" if s is None else str(s).replace("\t", "    ")
    s = "".join(ch if (ch.isprintable() or ch == " ") else "?" for ch in s)
    if w <= 0:
        return ""
    if len(s) <= w:
        return s
    if w <= len(ell):
        return s[:w]
    return s[:w - len(ell)] + ell


def pad(s, w):
    return fit(s, w).ljust(max(0, w))


def wrap(text, w):
    """Word-wrap, keeping explicit newlines. Never loses a long word."""
    if w <= 0:
        return []
    out = []
    for para in str(text).replace("\r", "").split("\n"):
        if not para.strip():
            out.append("")
            continue
        line = ""
        for word in para.split(" "):
            while len(word) > w:                # a URL or a hash: hard-split
                if line:
                    out.append(line)
                    line = ""
                out.append(word[:w])
                word = word[w:]
            if not line:
                line = word
            elif len(line) + 1 + len(word) <= w:
                line += " " + word
            else:
                out.append(line)
                line = word
        out.append(line)
    return out


def fmt_int(n):
    try:
        return f"{int(n):,}"
    except Exception:
        return "—"


def fmt_float(x, nd=4):
    try:
        f = float(x)
    except Exception:
        return "—"
    if f != f:                                  # NaN
        return "nan"
    return f"{f:.{nd}f}"


def fmt_dur(seconds):
    """Compact duration. None/negative/garbage -> "—"."""
    try:
        s = int(float(seconds))
    except Exception:
        return "—"
    if s < 0:
        return "—"
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m{s % 60:02d}s"
    if s < 86400:
        return f"{s // 3600}h{(s % 3600) // 60:02d}m"
    return f"{s // 86400}d{(s % 86400) // 3600:02d}h"


def rel_time(ts, now=None):
    """"3m ago" from an ISO string or a unix float. Unparseable -> "—"."""
    now = time.time() if now is None else now
    t = None
    if isinstance(ts, (int, float)):
        t = float(ts)
    elif isinstance(ts, str) and ts:
        s = ts.strip().replace("Z", "+00:00")
        try:
            dt = datetime.fromisoformat(s)
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=timezone.utc)
            t = dt.timestamp()
        except Exception:
            t = None
    if t is None:
        return "—"
    d = now - t
    if d < 0:
        return "in " + fmt_dur(-d)
    return fmt_dur(d) + " ago"


def bar(frac, w, full="█", empty="░"):
    """Progress bar. frac is clamped to [0,1]; a bad frac draws an empty bar."""
    if w <= 0:
        return ""
    try:
        f = float(frac)
    except Exception:
        f = 0.0
    if f != f:
        f = 0.0
    f = max(0.0, min(1.0, f))
    n = int(round(f * w))
    return full * n + empty * (w - n)


def pct(a, b):
    try:
        a, b = float(a), float(b)
    except Exception:
        return None
    if b <= 0:
        return None
    return a / b


# ── mail: threads ─────────────────────────────────────────────────────────

def thread_key(m):
    """A message's thread. Explicit `thread` wins; else its subject; else its
    own id — a message with neither is a thread of one, never merged with
    other subject-less mail."""
    t = (m.get("thread") or "").strip()
    if t:
        return "t:" + t
    s = (m.get("subject") or "").strip().lower()
    s = re.sub(r"^((re|fwd)\s*:\s*)+", "", s)   # repeated, or "re: re: x" forks its own thread
    if s:
        return "s:" + s
    return "i:" + str(m.get("id") or "")


def group_threads(msgs):
    """[msg] -> [thread], most-recently-active first. Each thread carries its
    messages oldest-first, an unread count, and who is in it."""
    by = {}
    for m in msgs or []:
        k = thread_key(m)
        th = by.get(k)
        if th is None:
            th = by[k] = {"key": k, "msgs": [], "unread": 0, "last_ts": "",
                          "parties": set(), "title": ""}
        th["msgs"].append(m)
    out = []
    for th in by.values():
        th["msgs"].sort(key=lambda m: str(m.get("ts") or ""))
        th["last_ts"] = str(th["msgs"][-1].get("ts") or "")
        th["unread"] = sum(1 for m in th["msgs"] if m.get("read") is False)
        parties = []
        for m in th["msgs"]:
            for who in (m.get("frm"), m.get("to")):
                if who and who not in parties:
                    parties.append(who)
        th["parties"] = parties
        title = ""
        for m in th["msgs"]:
            if (m.get("subject") or "").strip():
                title = m["subject"].strip()
                break
        if not title:
            title = (th["msgs"][-1].get("text") or "").strip().split("\n")[0]
        th["title"] = title or "(no subject)"
        out.append(th)
    out.sort(key=lambda t: t["last_ts"], reverse=True)
    return out


def unread_for(msgs, agent):
    """Ids in `msgs` that `agent` has not acked and that are addressed to them.

    `read` is only meaningful for the agent the inbox was computed for, so this
    reads the flag the loader attached and never guesses for a third party."""
    out = []
    for m in msgs or []:
        if m.get("to") not in (agent, mail.EVERYONE):
            continue
        if m.get("frm") == agent:
            continue
        if m.get("read") is False:
            out.append(m.get("id"))
    return [i for i in out if i]


def projects_of(msgs):
    """The project tags on the board, as a cycle for the `p` filter.

    The project field is the only thing in a message that narrows an audience
    to a repo, so it is what the board filters on — see the module docstring."""
    seen = set()
    blank = False
    for m in msgs or []:
        p = (m.get("project") or "").strip()
        if p:
            seen.add(p)
        else:
            blank = True
    out = [ALL_PROJECTS] + sorted(seen)
    if blank:
        out.append("(none)")
    return out


def filter_by_project(msgs, project):
    if not project or project == ALL_PROJECTS:
        return list(msgs or [])
    if project == "(none)":
        return [m for m in (msgs or []) if not (m.get("project") or "").strip()]
    return [m for m in (msgs or []) if (m.get("project") or "").strip() == project]


def is_broadcast(thread):
    """True when any message in the thread went to everyone — the 📢 marker."""
    return any(m.get("to") == mail.EVERYONE for m in (thread or {}).get("msgs", []))


def reply_defaults(thread, me):
    """to/subject/thread for a reply into `thread` from `me`."""
    if not thread or not thread.get("msgs"):
        return {"to": "codex", "subject": "", "thread": "", "project": ""}
    last = thread["msgs"][-1]
    to = last.get("frm") if last.get("frm") != me else last.get("to")
    if to == me or to not in mail.AGENTS:
        to = next((p for p in thread["parties"] if p in mail.AGENTS and p != me), "codex")
    subj = (last.get("subject") or thread.get("title") or "").strip()
    if subj:
        # Exactly one "re:", however many the incoming subject had carried —
        # a thread does not get longer in the subject line every round trip.
        subj = "re: " + re.sub(r"^((re|fwd)\s*:\s*)+", "", subj, flags=re.I)
    tval = (last.get("thread") or "").strip()
    if not tval and thread["key"].startswith("t:"):
        tval = thread["key"][2:]
    # A reply stays inside the thread's project — replying into a repo's
    # conversation must not silently re-scope it to wherever the operator
    # happens to be sitting.
    proj = ""
    for m in reversed(thread["msgs"]):
        if (m.get("project") or "").strip():
            proj = m["project"].strip()
            break
    return {"to": to, "subject": subj[:mail.MAX_SUBJECT], "thread": tval, "project": proj}


# ── trainers, metrics, configs ────────────────────────────────────────────

def run_of_cmd(cmd):
    """`--config configs/trio-sit5.yaml` -> `trio-sit5`. None if absent."""
    m = CONFIG_RE.search(cmd or "")
    if not m:
        return None
    return Path(m.group(1)).stem or None


def scan_trainers(proc_root="/proc", self_pid=None):
    """Live trainers, read from /proc — never `pgrep`.

    ⚠ `night_shift.py::train_pid()` greps for `train\\.py --config`, so any
    command line carrying that string reads as a live trainer. A subprocess
    call would put the pattern on OUR command line. Reading /proc puts it
    nowhere, and cannot self-match."""
    self_pid = os.getpid() if self_pid is None else self_pid
    out = []
    try:
        entries = sorted((d for d in os.listdir(proc_root) if d.isdigit()), key=int)
    except Exception:
        return out
    for d in entries:
        pid = int(d)
        if pid == self_pid:
            continue
        try:
            raw = Path(proc_root, d, "cmdline").read_bytes()
        except Exception:
            continue
        cmd = raw.replace(b"\0", b" ").decode("utf-8", "replace").strip()
        if not cmd or not TRAIN_RE.search(cmd):
            continue
        out.append({"pid": pid, "cmd": cmd, "run": run_of_cmd(cmd),
                    "island": "vision" if "train_vision.py" in cmd else "lm"})
    return out


def tail_lines(path, n=1, budget=262144):
    """Last n non-empty lines without reading the whole file."""
    p = Path(path)
    try:
        size = p.stat().st_size
    except Exception:
        return []
    if size == 0:
        return []
    want = min(size, budget)
    try:
        with p.open("rb") as fh:
            fh.seek(size - want)
            data = fh.read(want)
    except Exception:
        return []
    lines = [l for l in data.decode("utf-8", "replace").splitlines() if l.strip()]
    return lines[-n:]


def tail_json(path, n=1):
    out = []
    for line in tail_lines(path, n):
        try:
            row = json.loads(line)
        except Exception:
            continue                            # a torn tail line is skipped
        if isinstance(row, dict):
            out.append(row)
    return out


def config_total_steps(path):
    """`total_steps` out of a YAML config without a YAML parser — server.py has
    the same problem and solves it the same way (stdlib-only callers)."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except Exception:
        return None
    m = TOTAL_STEPS_RE.search(text)
    if not m:
        return None
    try:
        return int(m.group(1).replace("_", ""))
    except Exception:
        return None


def pid_alive(pid):
    try:
        pid = int(pid)
    except Exception:
        return False
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except Exception:
        return False
    return True


# ── peers: who else is working, and where ─────────────────────────────────

def agent_of_cmd(cmd):
    """"claude" / "codex" / None for a command line.

    ⚠ Exact basename match on argv[0]. Substring matching would count
    `codex-code-mode-host`, the shell snapshots Claude Code spawns (whose
    command line mentions ~/.claude/), and this very program — three ways to
    hallucinate a colleague."""
    cmd = (cmd or "").strip()
    if not cmd:
        return None
    argv0 = cmd.split(" ", 1)[0]
    base = Path(argv0).name
    agent = AGENT_EXE.get(base)
    if not agent:
        return None
    if any(flag in cmd for flag in NOT_A_SESSION):
        return None
    return agent


def proc_start_time(stat_text, btime, clk_tck=100):
    """Unix start time from /proc/<pid>/stat field 22.

    ⚠ Field 2 (comm) is parenthesised and may itself contain spaces, so the
    split has to start after the LAST ')' — splitting the whole line shifts
    every field for any process with a space in its name."""
    try:
        tail = stat_text[stat_text.rindex(")") + 1:].split()
        # after comm, field 3 is state -> starttime (field 22) is index 19
        ticks = float(tail[19])
        return float(btime) + ticks / float(clk_tck or 100)
    except Exception:
        return None


def _btime(proc_root="/proc"):
    try:
        for line in Path(proc_root, "stat").read_text().splitlines():
            if line.startswith("btime "):
                return float(line.split()[1])
    except Exception:
        pass
    return None


def repo_of_path(path, is_repo=None):
    """Nearest ancestor holding a .git; the path itself when there is none."""
    if not path:
        return ""
    if is_repo is None:
        def is_repo(p):
            return Path(p, ".git").exists()
    p = Path(path)
    for d in [p, *p.parents]:
        try:
            if is_repo(str(d)):
                return str(d)
        except Exception:
            break
    return str(p)


def scan_agent_sessions(proc_root="/proc", self_pid=None, now=None, btime=None,
                        clk_tck=100, is_repo=None):
    """Every live `claude` / `codex` session with the repo it is sitting in.

    `/proc/<pid>/cwd` is the authoritative answer to "which repo is this
    session working on" — a transcript path or a lifecycle row is a record of
    where it WAS. Reading a symlink writes nothing and needs no subprocess."""
    self_pid = os.getpid() if self_pid is None else self_pid
    now = time.time() if now is None else now
    btime = _btime(proc_root) if btime is None else btime
    out = []
    try:
        pids = sorted((d for d in os.listdir(proc_root) if d.isdigit()), key=int)
    except Exception:
        return out
    for d in pids:
        pid = int(d)
        if pid == self_pid:
            continue
        try:
            raw = Path(proc_root, d, "cmdline").read_bytes()
        except Exception:
            continue
        cmd = raw.replace(b"\0", b" ").decode("utf-8", "replace").strip()
        agent = agent_of_cmd(cmd)
        if not agent:
            continue
        try:
            cwd = os.readlink(str(Path(proc_root, d, "cwd")))
        except Exception:
            cwd = ""
        started = None
        if btime is not None:
            try:
                started = proc_start_time(Path(proc_root, d, "stat").read_text(),
                                          btime, clk_tck)
            except Exception:
                started = None
        out.append({"pid": pid, "agent": agent, "cwd": cwd,
                    "repo": repo_of_path(cwd, is_repo) if cwd else "",
                    "started": started,
                    "age": (now - started) if started else None,
                    "cmd": cmd})
    return out


def group_peers(sessions, here=None):
    """Sessions grouped by repo, the repo you are in first.

    `contended` marks a repo with more than one live session — the case the operator
    named: two agents editing the same tree without a shared conversation."""
    here = str(here) if here else ""
    by = {}
    for s in sessions or []:
        repo = s.get("repo") or s.get("cwd") or "?"
        g = by.setdefault(repo, {"repo": repo, "name": Path(repo).name or repo,
                                 "sessions": [], "claude": 0, "codex": 0})
        g["sessions"].append(s)
        if s.get("agent") in ("claude", "codex"):
            g[s["agent"]] += 1
    out = []
    for g in by.values():
        g["sessions"].sort(key=lambda s: (s.get("started") or 0))
        g["n"] = len(g["sessions"])
        g["contended"] = g["n"] > 1
        g["here"] = bool(here) and (here == g["repo"] or here.startswith(g["repo"] + os.sep))
        out.append(g)
    out.sort(key=lambda g: (not g["here"], -g["n"], g["name"]))
    return out


# ── judge ─────────────────────────────────────────────────────────────────

def judge_rows(by_kind):
    """{kind: trio_judge.status(kind)} -> flat display rows, one per shard."""
    rows = []
    for kind in sorted(by_kind or {}):
        st = by_kind[kind]
        if not isinstance(st, dict):
            rows.append({"kind": kind, "shard": "—", "error": str(st)})
            continue
        if st.get("error"):
            rows.append({"kind": kind, "shard": "—", "error": st["error"]})
            continue
        shards = st.get("shards") or {}
        if not shards:
            rows.append({"kind": kind, "shard": "—", "error": "no samples"})
            continue
        for sh in sorted(shards):
            s = shards[sh]
            rows.append({
                "kind": kind, "shard": sh,
                "units": s.get("units"), "judged": s.get("judged"),
                "frac": pct(s.get("judged"), s.get("units")),
                "families": dict(s.get("per_family") or {}),
                "multi": s.get("multi_family_units"),
                "agree": s.get("cross_family_agreement"),
            })
    return rows


# ── codex lifecycle ───────────────────────────────────────────────────────

def codex_events(db_path=None, limit=40):
    """Recent lifecycle rows. ⚠ read-only URI — this is a live DB owned by the
    Codex hook adapter and must never be locked or written by us."""
    p = Path(db_path or CODEX_DB)
    if not p.exists():
        return {"ok": False, "error": f"no lifecycle db at {p}", "rows": []}
    try:
        con = sqlite3.connect(f"file:{p}?mode=ro", uri=True, timeout=1.0)
        try:
            cur = con.execute(
                "select seq, recorded_at, session_id, cwd, event from events "
                "order by seq desc limit ?", (int(limit),))
            rows = [{"seq": r[0], "at": r[1], "session": r[2], "cwd": r[3], "event": r[4]}
                    for r in cur.fetchall()]
            total = con.execute("select count(*) from events").fetchone()[0]
        finally:
            con.close()
    except Exception as exc:
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}", "rows": []}
    return {"ok": True, "rows": rows, "total": total, "path": str(p)}


def codex_sessions(rows, limit=6):
    """Collapse an event list into per-session summaries, newest first."""
    by = {}
    for r in rows or []:
        sid = r.get("session") or "?"
        s = by.setdefault(sid, {"session": sid, "n": 0, "last": "", "first": "",
                                "cwd": r.get("cwd") or "", "event": r.get("event") or ""})
        s["n"] += 1
        at = str(r.get("at") or "")
        if not s["last"] or at > s["last"]:
            s["last"] = at
            s["event"] = r.get("event") or ""
            s["cwd"] = r.get("cwd") or s["cwd"]
        if not s["first"] or at < s["first"]:
            s["first"] = at
    return sorted(by.values(), key=lambda s: s["last"], reverse=True)[:limit]


# ══════════════════════════════════════════════════════════════════════════
# collectors — each returns {"ok": bool, ...} and NEVER raises
# ══════════════════════════════════════════════════════════════════════════


def ring_token():
    """⚠ Returns the token; callers put it in a header and nowhere else. It is
    never rendered, never logged, never in an error string."""
    cfg = PROJECT_ROOT / ".apollo" / "config"
    try:
        for line in cfg.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.strip().startswith("token"):
                return line.split("=", 1)[1].strip()
    except Exception:
        pass
    return ""


def collect_den(timeout=2.0):
    """The Den's Oracorker console. Down is a normal state, not an error."""
    import urllib.error
    import urllib.request
    tok = ring_token()
    if not tok:
        return {"ok": False, "error": "no ring token in demo/.apollo/config"}
    req = urllib.request.Request(DEN_URL + "/api/oracorker",
                                 headers={RING_HEADER: tok})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as fh:
            data = json.loads(fh.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as exc:
        return {"ok": False, "error": f"den {DEN_URL} HTTP {exc.code}"}
    except Exception as exc:
        return {"ok": False, "error": f"den {DEN_URL} unreachable ({type(exc).__name__})"}
    if not isinstance(data, dict):
        return {"ok": False, "error": "den returned a non-object"}
    data["ok"] = True
    return data


def collect_trainers():
    """Live trainers + the last metrics line each one wrote."""
    out = []
    for t in scan_trainers():
        run = t.get("run")
        rec = dict(t)
        rec["metrics"] = None
        rec["metrics_path"] = None
        rec["total"] = None
        if run:
            mp = SIBLING / "runs" / run / "metrics.jsonl"
            rec["metrics_path"] = str(mp)
            rows = tail_json(mp, 1)
            if rows:
                rec["metrics"] = rows[0]
            m = CONFIG_RE.search(t["cmd"] or "")
            if m:
                cfg = Path(m.group(1))
                if not cfg.is_absolute():
                    cfg = SIBLING / cfg
                rec["config"] = str(cfg)
                rec["total"] = config_total_steps(cfg)
        out.append(rec)
    lock = SIBLING / ".train.lock"
    lk = {"path": str(lock), "exists": lock.exists(), "pid": None, "alive": None}
    if lk["exists"]:
        try:
            raw = lock.read_text(encoding="utf-8", errors="replace").strip()
            lk["pid"] = int(raw) if raw.isdigit() else None
        except Exception:
            lk["pid"] = None
        if lk["pid"]:
            lk["alive"] = pid_alive(lk["pid"])
    return {"ok": True, "trainers": out, "lock": lk}


def collect_judge():
    """Judge coverage. Imports the trainer/scripts/trio_judge.py — stdlib-only and
    ~0.1s per kind (measured), so it is cheap enough to poll."""
    sp = str(SIBLING / "scripts")
    if sp not in sys.path:
        sys.path.insert(0, sp)
    try:
        import trio_judge                                     # noqa: F401
    except Exception as exc:
        return {"ok": False, "error": f"trio_judge unavailable ({type(exc).__name__}: {exc})"}
    out = {}
    for kind in JUDGE_KINDS:
        try:
            out[kind] = trio_judge.status(kind)
        except Exception as exc:
            out[kind] = {"error": f"{type(exc).__name__}: {exc}"}
    return {"ok": True, "kinds": out}


def collect_todo(cwd=None):
    try:
        import liljack_todo as T
    except Exception as exc:
        return {"ok": False, "error": f"liljack_todo unavailable ({type(exc).__name__})"}
    try:
        path = T.find_todo(cwd or str(PROJECT_ROOT))
        if not path:
            return {"ok": False, "error": "no TODO.md above this directory"}
        parsed = T.parse_todo(path)
        return {"ok": True, "path": str(path), "line": T.status_line(parsed),
                "next": [i["text"] for i in T.next_pending(parsed, 4)],
                "done": sum(1 for m in parsed["milestones"] for i in m["items"] if i["done"]),
                "total": sum(len(m["items"]) for m in parsed["milestones"]),
                "milestone": (parsed["milestones"][0]["title"] if parsed["milestones"] else "")}
    except Exception as exc:
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}"}


def collect_mood():
    out = {"ok": False, "error": "", "mood": "", "face": "", "traits": [], "codex": None}
    try:
        import liljack
        st = liljack.read_dsl()
        out["mood"] = st.get("mood") or ""
        out["face"] = liljack.MOOD_FACE.get(out["mood"], "(･_･)")
        out["traits"] = st.get("traits") or []
        out["impressions"] = st.get("impressions") or {}
        out["ok"] = True
    except Exception as exc:
        out["error"] = f"liljack.dsl unreadable ({type(exc).__name__})"
    try:
        import liljack_codex
        out["codex"] = liljack_codex.status()
    except Exception as exc:
        out["codex_error"] = f"{type(exc).__name__}: {exc}"
    return out


def collect_mail(root=None, limit=200):
    """Every message on the board, assembled from the public API only.

    `inbox()` answers "mail for X"; the board wants ALL traffic. Every message
    has a sender among the three agents, so the union of `sent()` over the
    agents IS the whole store — and it costs no new reader on an append-only
    file two harnesses are writing. the operator's read flags come from his inbox."""
    try:
        seen = {}
        for a in mail.AGENTS:
            for m in mail.sent(a, limit=limit, root=root):
                seen[m.get("id")] = dict(m)
        for a in mail.AGENTS:
            for m in mail.inbox(a, unread_only=False, limit=limit, root=root):
                mid = m.get("id")
                if mid in seen:
                    seen[mid].setdefault("read_by_flags", {})[a] = bool(m.get("read"))
        msgs = []
        for m in seen.values():
            flags = m.pop("read_by_flags", {})
            m["read"] = flags.get("operator") if "operator" in flags else None
            m["flags"] = flags
            msgs.append(m)
        msgs.sort(key=lambda m: str(m.get("ts") or ""))
        unread = {a: len(mail.inbox(a, unread_only=True, limit=limit, root=root))
                  for a in mail.AGENTS}
        return {"ok": True, "msgs": msgs, "unread": unread,
                "path": str(mail._paths(root)[0])}
    except Exception as exc:
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}", "msgs": [],
                "unread": {}}


def collect_peers(here=None):
    try:
        sessions = scan_agent_sessions()
    except Exception as exc:
        return {"ok": False, "error": f"/proc scan failed ({type(exc).__name__})",
                "groups": [], "sessions": []}
    here = here or os.getcwd()
    groups = group_peers(sessions, here=repo_of_path(here))
    return {"ok": True, "sessions": sessions, "groups": groups,
            "here": repo_of_path(here), "path": "/proc/<pid>/cwd"}


def collect_atlas(n=25):
    p = SIBLING / "runs" / "atlas.jsonl"
    rows = tail_json(p, n)
    if not rows:
        return {"ok": False, "error": f"no records in {p}", "rows": [], "path": str(p)}
    rows.reverse()
    return {"ok": True, "rows": rows, "path": str(p)}


def collect_walks_local(limit=8):
    """Walk state straight from the files, so the RUNS tab still works when the
    Den is down. Counts come from points.jsonl, not from a cached summary."""
    root = SIBLING / "runs" / "walks"
    if not root.is_dir():
        return {"ok": False, "error": f"no walks dir at {root}", "walks": []}
    dirs = sorted((d for d in root.iterdir() if d.is_dir()),
                  key=lambda d: d.stat().st_mtime, reverse=True)[:limit]
    out = []
    for d in dirs:
        w = {"id": d.name, "status": "?", "phase": "?", "mode": "?", "steps": None,
             "budget": None, "done": 0, "running": 0}
        try:
            j = json.loads((d / "walk.json").read_text(encoding="utf-8"))
            w.update({k: j.get(k) for k in ("status", "phase", "mode", "steps", "budget")})
        except Exception:
            w["status"] = "unreadable walk.json"
        pts = d / "points.jsonl"
        if pts.exists():
            done = running = 0
            for line in pts.read_text(encoding="utf-8", errors="replace").splitlines():
                if not line.strip():
                    continue
                try:
                    r = json.loads(line)
                except Exception:
                    continue
                st = r.get("status") or ("done" if r.get("val_loss") is not None else "")
                if st == "running":
                    running += 1
                elif st:
                    done += 1
            w["done"], w["running"] = done, running
        w["mtime"] = d.stat().st_mtime
        out.append(w)
    return {"ok": True, "walks": out, "path": str(root)}


def collect_host():
    """Load + package temperature. ⚠ The measured thermal ceiling on this box is
    99 °C at 34 workers (crit 100); 83 °C is the healthy all-core figure, so the
    number is worth a colour."""
    out = {"ok": True, "load1": None, "cores": None, "package_c": None, "uptime": None}
    try:
        out["load1"] = os.getloadavg()[0]
    except Exception:
        pass
    try:
        out["cores"] = os.cpu_count()
    except Exception:
        pass
    try:
        out["uptime"] = float(Path("/proc/uptime").read_text().split()[0])
    except Exception:
        pass
    try:
        for hw in sorted(Path("/sys/class/hwmon").glob("hwmon*")):
            name = (hw / "name").read_text().strip() if (hw / "name").exists() else ""
            if name not in ("coretemp", "k10temp", "zenpower"):
                continue
            for lbl in sorted(hw.glob("temp*_label")):
                if "Package" in lbl.read_text() or "Tctl" in lbl.read_text():
                    val = lbl.parent / lbl.name.replace("_label", "_input")
                    out["package_c"] = int(val.read_text().strip()) / 1000.0
                    break
            if out["package_c"] is not None:
                break
    except Exception:
        pass
    return out


# ══════════════════════════════════════════════════════════════════════════
# snapshot cache — per-source TTL, refreshed off the UI thread
# ══════════════════════════════════════════════════════════════════════════

SOURCES = {
    # name        (fn,               ttl seconds)
    "mail":       (collect_mail,     1.0),
    "peers":      (collect_peers,    2.0),
    "trainers":   (collect_trainers, 2.0),
    "den":        (collect_den,      3.0),
    "host":       (collect_host,     3.0),
    "judge":      (collect_judge,    20.0),
    "todo":       (collect_todo,     30.0),
    "mood":       (collect_mood,     5.0),
    "codex":      (lambda: codex_events(limit=60), 4.0),
    "atlas":      (lambda: collect_atlas(25), 15.0),
    "walks":      (collect_walks_local, 10.0),
    # "chat" is added by main(): it needs the socket path (and --demo's fake)
}


class Cache:
    """Per-source TTL cache with an injectable clock, so a test can drive it."""

    def __init__(self, sources=None, clock=time.time):
        self.sources = dict(sources if sources is not None else SOURCES)
        self.clock = clock
        self.data = {}
        self.at = {}
        self.lock = threading.Lock()

    def due(self, name, now=None):
        now = self.clock() if now is None else now
        if name not in self.sources:
            return False
        if name not in self.at:
            return True
        return (now - self.at[name]) >= self.sources[name][1]

    def refresh(self, name, force=False):
        if not force and not self.due(name):
            return False
        fn = self.sources[name][0]
        try:
            val = fn()
        except Exception as exc:                # a collector must never kill the app
            val = {"ok": False, "error": f"collector {name} raised "
                                         f"{type(exc).__name__}: {exc}"}
        with self.lock:
            self.data[name] = val
            self.at[name] = self.clock()
        return True

    def refresh_due(self):
        for name in self.sources:
            self.refresh(name)

    def get(self, name, default=None):
        with self.lock:
            return self.data.get(name, default)

    def snapshot(self):
        with self.lock:
            return dict(self.data), dict(self.at)

    def age(self, name, now=None):
        now = self.clock() if now is None else now
        with self.lock:
            t = self.at.get(name)
        return None if t is None else now - t


# ══════════════════════════════════════════════════════════════════════════
# views — snapshot -> [(style, text)].  No curses here, on purpose.
# ══════════════════════════════════════════════════════════════════════════
#
# styles: hdr key ok warn err dim sel unread bar plain

def L(style, text=""):
    return (style, text)


def panel(title, src="", w=80):
    """A HUI-style panel header: title in a rule, provenance in dim on the right."""
    left = f"─ {title} "
    if src:
        right = f" {src} ─"
        fill = max(1, w - len(left) - len(right) - 1)
        return [L("hdr", "┌" + left + "─" * fill + right)]
    fill = max(1, w - len(left) - 1)
    return [L("hdr", "┌" + left + "─" * fill)]


def view_status(snap, w=100, now=None):
    """The STATUS tab. Every line names the file or endpoint it came from."""
    now = time.time() if now is None else now
    out = []
    tr = snap.get("trainers") or {}
    den = snap.get("den") or {}

    # ── trainer ───────────────────────────────────────────────────────────
    out += panel("TRAINER", "/proc + the trainer/runs/<run>/metrics.jsonl", w)
    trainers = tr.get("trainers") or []
    if not trainers:
        out.append(L("dim", "  no trainer — nothing matches 'train(_vision).py --config' in /proc"))
    for t in trainers:
        m = (t.get("metrics") or {}).get("metrics") or {}
        step = (t.get("metrics") or {}).get("step")
        total = t.get("total")
        head = f"  ▶ {t.get('run') or '?'}  [{t.get('island')}]  pid {t.get('pid')}"
        out.append(L("ok", head))
        if step is None:
            out.append(L("warn", f"    no metrics yet — {t.get('metrics_path') or 'no run dir'} "
                                 "(starting / encoding corpus)"))
        else:
            f = pct(step, total)
            prog = f"step {fmt_int(step)}/{fmt_int(total)}" if total else f"step {fmt_int(step)} (total_steps not in config)"
            if f is not None:
                prog += f"  {f * 100:5.1f}% {bar(f, 18)}"
            out.append(L("plain", "    " + prog))
            row = (f"    loss {fmt_float(m.get('loss'))}"
                   f"  lr {fmt_float(m.get('lr'), 8)}"
                   f"  {fmt_int(m.get('tokens_per_sec'))} tok/s"
                   f"  |grad| {fmt_float(m.get('grad_norm'))}")
            out.append(L("plain", row))
            wrote = (t.get("metrics") or {}).get("timestamp")
            age = (now - float(wrote)) if isinstance(wrote, (int, float)) else None
            style = "warn" if (age is not None and age > 180) else "dim"
            out.append(L(style, f"    elapsed {fmt_dur((t.get('metrics') or {}).get('elapsed_time'))}"
                                f" · last metrics line {rel_time(wrote, now)}"))
    lk = tr.get("lock") or {}
    if not lk.get("exists"):
        out.append(L("dim", f"  lock: absent ({lk.get('path','?')}) — the GPU is free"))
    elif lk.get("pid") and lk.get("alive"):
        out.append(L("dim", f"  lock: held by pid {lk['pid']} (alive)"))
    elif lk.get("pid"):
        out.append(L("err", f"  lock: STALE — pid {lk['pid']} is gone ({lk.get('path')})"))
    else:
        out.append(L("warn", f"  lock: present, unparseable pid ({lk.get('path')})"))
    out.append(L("plain", ""))

    # ── den / oracorker ───────────────────────────────────────────────────
    out += panel("ORACORKER (den 7700)", "GET /api/oracorker", w)
    if not den.get("ok"):
        out.append(L("err", "  " + (den.get("error") or "den 7700 unreachable")))
        out.append(L("dim", "  timers, walk phases and ladder rungs are unavailable "
                            "— start den7700 (see CLAUDE.md)"))
    else:
        timers = den.get("timers") or []
        if not timers:
            out.append(L("dim", "  no timers reported"))
        for t in timers:
            act = t.get("active")
            st = "ok" if act else "dim"
            when = f"next in {fmt_dur(t.get('next_in_s'))}" if t.get("next_in_s") is not None else "next —"
            flag = " ⚠can-launch-trainer" if t.get("can_launch_trainer") else ""
            out.append(L(st, f"  {'●' if act else '○'} {pad(t.get('unit','?'), 26)} "
                             f"{'active' if act else 'inactive'}  {when}{flag}"))
        live = den.get("live") or []
        for lv in live:
            out.append(L("ok", f"  den live: {lv.get('run')} step {fmt_int(lv.get('step'))}"
                               f"/{fmt_int(lv.get('total'))} loss {fmt_float(lv.get('loss'))}"
                               f" val {fmt_float(lv.get('val'))}"))
        prep = [p for p in (den.get("prep") or []) if p.get("state") == "active"]
        for p in prep[:4]:
            out.append(L("dim", f"  prep unit: {p.get('unit')} {p.get('sub')} "
                                f"· {fmt_dur(p.get('elapsed_s'))}"))
        stale = [s for wk in (den.get("walks") or []) for s in (wk.get("stale_running") or [])]
        if stale:
            out.append(L("warn", f"  ⚠ {len(stale)} stale 'running' marker(s) in points.jsonl "
                                 "— a status lie, not a live probe"))
    out.append(L("plain", ""))

    # ── judge ─────────────────────────────────────────────────────────────
    jd = snap.get("judge") or {}
    out += panel("JUDGE COVERAGE", "the trainer/scripts/trio_judge.py status", w)
    if not jd.get("ok"):
        out.append(L("err", "  " + (jd.get("error") or "judge status unavailable")))
    else:
        rows = judge_rows(jd.get("kinds") or {})
        if not rows:
            out.append(L("dim", "  no judge samples"))
        for r in rows:
            if r.get("error"):
                out.append(L("dim", f"  {pad(r['kind'], 8)} {pad(r['shard'], 10)} {r['error']}"))
                continue
            f = r.get("frac")
            fams = " ".join(f"{k}={v}" for k, v in sorted((r.get("families") or {}).items())) or "—"
            ag = r.get("agree")
            agree = f" agree {ag:.2f}" if isinstance(ag, (int, float)) else ""
            style = "ok" if (f is not None and f >= 0.999) else ("warn" if (f or 0) < 0.5 else "plain")
            out.append(L(style, f"  {pad(r['kind'], 7)} {pad(r['shard'], 8)}"
                                f" {fmt_int(r.get('judged')):>6}/{fmt_int(r.get('units')):<6}"
                                f" {bar(f or 0, 12)} {'' if f is None else f'{f*100:5.1f}%'}"
                                f"  {fams}{agree}"))
    out.append(L("plain", ""))

    # ── walks ─────────────────────────────────────────────────────────────
    out += panel("WALKS", "den /api/oracorker · the trainer/runs/walks/*/walk.json", w)
    dw = den.get("walks") if den.get("ok") else None
    if dw:
        for wk in dw[:6]:
            c = wk.get("counts") or {}
            counts = " ".join(f"{k}={v}" for k, v in sorted(c.items())) or "—"
            best = wk.get("best")
            b = f" best {fmt_float(best)}" if isinstance(best, (int, float)) else ""
            style = "ok" if wk.get("status") == "active" else "dim"
            out.append(L(style, f"  {pad(wk.get('id','?'), 22)} {pad(wk.get('status','?'), 9)}"
                                f" phase {pad(wk.get('phase','?'), 3)} {pad(wk.get('mode','?'), 7)}"
                                f" steps {pad(fmt_int(wk.get('steps')), 6)} · {counts}{b}"))
    else:
        lw = snap.get("walks") or {}
        if not lw.get("ok"):
            out.append(L("err", "  " + (lw.get("error") or "no walk state")))
        else:
            out.append(L("dim", "  (den down — read from walk.json/points.jsonl)"))
            for wk in (lw.get("walks") or [])[:6]:
                style = "ok" if wk.get("status") == "active" else "dim"
                out.append(L(style, f"  {pad(wk.get('id','?'), 22)} {pad(str(wk.get('status')), 9)}"
                                    f" phase {pad(str(wk.get('phase')), 3)}"
                                    f" {wk.get('done')} done / {wk.get('running')} running"
                                    f" of budget {wk.get('budget')}"))
    out.append(L("plain", ""))

    # ── todo + host ───────────────────────────────────────────────────────
    td = snap.get("todo") or {}
    out += panel("TODO", (td.get("path") or "TODO.md"), w)
    if not td.get("ok"):
        out.append(L("err", "  " + (td.get("error") or "no TODO.md")))
    else:
        out.append(L("ok", "  " + fit(td.get("line") or "", w - 4)))
        for i, item in enumerate(td.get("next") or []):
            out.append(L("plain" if i == 0 else "dim", "   " + ("▸ " if i == 0 else "· ")
                         + fit(item, w - 6)))
    out.append(L("plain", ""))
    h = snap.get("host") or {}
    out += panel("HOST", "/proc/loadavg · /sys/class/hwmon", w)
    tc = h.get("package_c")
    tstyle = "plain"
    if isinstance(tc, (int, float)):
        tstyle = "err" if tc >= 95 else ("warn" if tc >= 88 else "ok")
    out.append(L(tstyle, f"  load1 {fmt_float(h.get('load1'), 2)}"
                         f" / {h.get('cores') or '?'} cores"
                         f"  ·  package "
                         f"{(f'{tc:.0f} °C' if isinstance(tc, (int, float)) else 'unreadable')}"
                         f"  ·  up {fmt_dur(h.get('uptime'))}"))
    if isinstance(tc, (int, float)) and tc >= 88:
        out.append(L("warn", "  ⚠ measured ceiling on this box: 99 °C at 34 workers (crit 100); "
                             "18 physical cores settle at ~83 °C"))
    return out


def view_board_list(threads, sel, w, me="operator"):
    """The thread column."""
    out = []
    if not threads:
        out.append(L("dim", " no messages yet"))
        return out
    for i, th in enumerate(threads):
        mark = "●" if th["unread"] else " "
        cast = "!" if is_broadcast(th) else " "   # ! = went to `all`
        who = "/".join(p[:2] for p in th["parties"] if p != mail.EVERYONE)
        head = f"{mark}{cast}{pad(who, 8)} {fit(th['title'], max(4, w - 22))}"
        line = f"{pad(head, max(0, w - 8))}{rel_time(th['last_ts']).replace(' ago', ''):>7}"
        style = "sel" if i == sel else ("unread" if th["unread"] else "plain")
        out.append(L(style, line))
    return out


def view_board_thread(thread, w, me="operator"):
    """The message column for the selected thread."""
    out = []
    if not thread:
        out.append(L("dim", " select a thread — ↑/↓ or j/k, Enter to mark read"))
        return out
    out.append(L("hdr", fit(thread["title"], w)))
    projs = sorted({(m.get("project") or "").strip() for m in thread["msgs"]} - {""})
    out.append(L("dim", f"{len(thread['msgs'])} message(s) · "
                        f"{' ↔ '.join(thread['parties'])} · key {thread['key']}"
                        + (f" · project {', '.join(projs)}" if projs else " · no project tag")))
    if is_broadcast(thread):
        out.append(L("warn", "  ! broadcast thread — a message here went to `all`, "
                             "so every session of both harnesses sees it"))
    out.append(L("plain", ""))
    for m in thread["msgs"]:
        unread = m.get("read") is False
        head = (f"{'●' if unread else '·'} {m.get('frm','?')} → {m.get('to','?')}"
                f"  {str(m.get('ts',''))[:16].replace('T',' ')}"
                f"  {rel_time(m.get('ts'))}")
        rb = m.get("read_by") or []
        if rb:
            head += f"  read by {','.join(rb)}"
        out.append(L("unread" if unread else "key", fit(head, w)))
        if m.get("project"):
            out.append(L("dim", f"  project {m['project']}"
                                + (f" · thread {m['thread']}" if m.get("thread") else "")))
        for line in wrap(m.get("text") or "", max(8, w - 2)):
            out.append(L("plain", "  " + line))
        out.append(L("dim", f"  id {m.get('id','')}"))
        out.append(L("plain", ""))
    return out


def view_peers(snap, w=100, now=None):
    """Who is working right now, and on which repo — the operator's coordination view."""
    now = time.time() if now is None else now
    out = []
    pr = snap.get("peers") or {}
    out += panel("LIVE SESSIONS — who is on which repo", "/proc/<pid>/cwd", w)
    if not pr.get("ok"):
        out.append(L("err", "  " + (pr.get("error") or "cannot read /proc")))
    else:
        groups = pr.get("groups") or []
        if not groups:
            out.append(L("dim", "  no live claude/codex session found in /proc "
                                "(this app does not count itself)"))
        for g in groups:
            mark = "▸" if g.get("here") else " "
            style = "warn" if g["contended"] else ("ok" if g.get("here") else "plain")
            note = ""
            if g["contended"]:
                note = f"  ⚠ {g['n']} sessions share this tree — coordinate on the BOARD"
            out.append(L(style, f"  {mark} {pad(g['name'], 18)} "
                                f"claude×{g['claude']} codex×{g['codex']}{note}"))
            for s in g["sessions"]:
                out.append(L("dim", f"      {pad(s['agent'], 7)} pid {pad(str(s['pid']), 8)}"
                                    f" up {pad(fmt_dur(s.get('age')), 8)}"
                                    f" {fit(s.get('cwd') or '?', max(10, w - 40))}"))
        out.append(L("dim", "  ⚠ mail addresses AGENTS, not sessions: `claude` reaches every "
                            "Claude session. Tag the `project` and reply in-thread."))
    out.append(L("plain", ""))

    mood = snap.get("mood") or {}
    out += panel("LILJACK", "orc_data/liljack.dsl", w)
    if not mood.get("ok"):
        out.append(L("err", "  " + (mood.get("error") or "liljack.dsl unreadable")))
    else:
        out.append(L("ok", f"  {mood.get('face','')}  mood: {mood.get('mood','?')}"))
        out.append(L("dim", "  traits: " + fit(", ".join(mood.get("traits") or []) or "—", w - 12)))
        imps = mood.get("impressions") or {}
        if imps:
            s = " ".join(f"{k}={v.get('assessment')}({v.get('confidence')})"
                         for k, v in sorted(imps.items()))
            out.append(L("dim", "  impressions: " + fit(s, w - 16)))
    cs = mood.get("codex")
    if isinstance(cs, dict):
        out.append(L("plain", ""))
        out.append(L("key", f"  codex harness: {cs.get('event','?')} · session "
                            f"{str(cs.get('session_id',''))[:8]} · mood {cs.get('mood','?')} "
                            f"{cs.get('face','')} · {rel_time(cs.get('observed_at'), now)}"))
        if cs.get("quip"):
            out.append(L("dim", f"  \"{fit(cs['quip'], w - 6)}\""))
    elif mood.get("codex_error"):
        out.append(L("dim", "  codex status: " + mood["codex_error"]))
    out.append(L("plain", ""))

    ml = snap.get("mail") or {}
    out += panel("MAIL COUNTERS", ml.get("path") or "~/.cache/liljack/mailbox.jsonl", w)
    if not ml.get("ok"):
        out.append(L("err", "  " + (ml.get("error") or "mailbox unreadable")))
    else:
        un = ml.get("unread") or {}
        msgs = ml.get("msgs") or []
        for a in mail.AGENTS:
            sent_n = sum(1 for m in msgs if m.get("frm") == a)
            n = un.get(a, 0)
            out.append(L("unread" if n else "plain",
                         f"  {pad(a, 8)} unread {n:>3}   sent {sent_n:>3}"))
        out.append(L("dim", f"  {len(msgs)} message(s) on the board"))
    out.append(L("plain", ""))

    cx = snap.get("codex") or {}
    out += panel("CODEX LIFECYCLE", str(CODEX_DB) + " (read-only)", w)
    if not cx.get("ok"):
        out.append(L("err", "  " + (cx.get("error") or "lifecycle db unavailable")))
    else:
        out.append(L("dim", f"  {fmt_int(cx.get('total'))} events recorded"))
        for s in codex_sessions(cx.get("rows") or [], 4):
            out.append(L("key", f"  {s['session'][:8]} {pad(s['event'], 16)} "
                                f"{rel_time(s['last'], now):>10}  "
                                f"{fit(Path(s['cwd']).name if s['cwd'] else '?', 18)}"
                                f"  ({s['n']} of the last {len(cx.get('rows') or [])})"))
        out.append(L("plain", ""))
        for r in (cx.get("rows") or [])[:12]:
            out.append(L("dim", f"  {str(r.get('at',''))[11:19]} {pad(r.get('event',''), 18)}"
                                f" {str(r.get('session',''))[:8]}"
                                f" {fit(Path(r.get('cwd') or '?').name, 20)}"))
    return out


def view_runs(snap, w=100, now=None):
    now = time.time() if now is None else now
    out = []
    at = snap.get("atlas") or {}
    out += panel("ATLAS — newest runs", at.get("path") or "the trainer/runs/atlas.jsonl", w)
    if not at.get("ok"):
        out.append(L("err", "  " + (at.get("error") or "atlas unreadable")))
    else:
        out.append(L("key", f"  {pad('run', 34)}{pad('island', 8)}{pad('steps', 9)}"
                            f"{pad('best_val', 10)}{pad('val_loss', 10)}{pad('acc1', 8)}era"))
        for r in at["rows"]:
            era = r.get("era")
            era_id = era.get("id") if isinstance(era, dict) else (era or "")
            a1 = r.get("acc1")
            out.append(L("plain",
                         f"  {pad(r.get('run', '?'), 34)}"
                         f"{pad(r.get('island') or '—', 8)}"
                         f"{pad(fmt_int(r.get('steps')), 9)}"
                         f"{pad(fmt_float(r.get('best_val')), 10)}"
                         f"{pad(fmt_float(r.get('val_loss')), 10)}"
                         f"{pad(f'{a1*100:.1f}%' if isinstance(a1, (int, float)) else '—', 8)}"
                         f"{fit(era_id, max(4, w - 81))}"))
        out.append(L("dim", "  ⚠ rank on val_loss; compare across runs by bits/byte WITHIN an era. "
                            "`bpc` in this file is bits per TOKEN."))
    out.append(L("plain", ""))
    lw = snap.get("walks") or {}
    out += panel("WALK DIRECTORIES", lw.get("path") or "the trainer/runs/walks", w)
    if not lw.get("ok"):
        out.append(L("err", "  " + (lw.get("error") or "no walks")))
    else:
        for wk in lw.get("walks") or []:
            style = "ok" if wk.get("status") == "active" else "dim"
            out.append(L(style, f"  {pad(wk.get('id','?'), 24)} {pad(str(wk.get('status')), 10)}"
                                f" phase {pad(str(wk.get('phase')), 3)} {pad(str(wk.get('mode')), 8)}"
                                f" {wk.get('done')} done/{wk.get('running')} running"
                                f" of {wk.get('budget')} · {rel_time(wk.get('mtime'), now)}"))
    return out


def ribbon(snap, w=100, now=None):
    """The bottom status line — lilJack's face, the same shape as the statusline."""
    now = time.time() if now is None else now
    mood = snap.get("mood") or {}
    tr = snap.get("trainers") or {}
    td = snap.get("todo") or {}
    ml = snap.get("mail") or {}
    den = snap.get("den") or {}
    bits = []
    bits.append(f"{mood.get('face', '(･_･)')} {mood.get('mood', '?')}")
    trs = tr.get("trainers") or []
    if trs:
        t = trs[0]
        step = (t.get("metrics") or {}).get("step")
        bits.append(f"▶{t.get('run')}@{fmt_int(step) if step is not None else 'start'}")
    else:
        bits.append("▶none")
    bits.append("den✓" if den.get("ok") else "den✗")
    pr = snap.get("peers") or {}
    if pr.get("ok"):
        here = next((g for g in (pr.get("groups") or []) if g.get("here")), None)
        if here:
            bits.append(("⚠" if here["contended"] else "") +
                        f"👥{here['claude']}c/{here['codex']}x@{here['name']}")
        else:
            bits.append(f"👥{len(pr.get('sessions') or [])}")
    if td.get("ok"):
        bits.append(f"☑{td.get('done')}/{td.get('total')}")
    un = (ml.get("unread") or {}).get("operator", 0) if ml.get("ok") else 0
    bits.append(f"✉{un}" if un else "✉0")
    return fit(" · ".join(bits), w)


HELP = [
    ("q  /  Ctrl-C", "quit"),
    ("1 2 3 4 5", "jump to BOARD / STATUS / PEERS / RUNS / CHAT"),
    ("Tab / Shift-Tab", "next / previous tab (on CHAT: Tab cycles the focused tile)"),
    ("CHAT: s t f", "tiles side-by-side / stacked / focus one"),
    ("CHAT: Enter / i", "type into the focused tile; Enter sends as operator, Esc cancels"),
    ("CHAT: c / g", "take control as operator / give control (daemon-approved targets only)"),
    ("j k  ↑ ↓", "move the selection"),
    ("g G", "first / last"),
    ("PgUp PgDn", "scroll the right pane"),
    ("Enter", "open the thread and mark its mail read (acks as operator)"),
    ("c", "compose a new message"),
    ("r", "reply into the selected thread"),
    ("a", "ack every unread message addressed to operator"),
    ("u", "toggle: all threads / unread only"),
    ("p", "cycle the project filter — one repo's traffic at a time"),
    ("R", "force-refresh every source now"),
    ("?", "this help"),
]


# ══════════════════════════════════════════════════════════════════════════
# app state — pure transitions, so the key handling is testable
# ══════════════════════════════════════════════════════════════════════════


class State:
    def __init__(self, tab=0, me="operator"):
        self.tab = max(0, min(len(TABS) - 1, int(tab)))
        self.me = me
        self.sel = {t: 0 for t in TABS}
        self.scroll = {t: 0 for t in TABS}
        self.unread_only = False
        self.project = ALL_PROJECTS
        self.help = False
        self.flash = ""
        self.flash_at = 0.0
        # CHAT
        self.chat_focus = 0
        self.chat_layout = "side"
        self.chat_typing = False
        self.chat_input = ""
        self.chat_seen = {}             # agent -> newest transcript ts seen
        self.chat_modal = None          # {"kind": "give"|"spawn", ...}
        self.chat_notice = ""           # last refusal / hint, above the input line

    def chat_focus_next(self, n, d=1):
        if n <= 0:
            self.chat_focus = 0
            return 0
        self.chat_focus = (self.chat_focus + d) % n
        return self.chat_focus

    def cycle_project(self, options, d=1):
        """Advance the board's project filter through the tags actually present."""
        opts = list(options or [ALL_PROJECTS])
        if self.project not in opts:
            self.project = opts[0]
            return self.project
        i = opts.index(self.project)
        self.project = opts[(i + d) % len(opts)]
        return self.project

    @property
    def tab_name(self):
        return TABS[self.tab]

    def set_tab(self, i):
        if 0 <= i < len(TABS):
            self.tab = i
        return self.tab

    def next_tab(self, d=1):
        self.tab = (self.tab + d) % len(TABS)
        return self.tab

    def move(self, n, delta):
        """Move the current tab's selection within [0, n-1]; clamps, never wraps."""
        cur = self.sel.get(self.tab_name, 0)
        if n <= 0:
            self.sel[self.tab_name] = 0
            return 0
        cur = max(0, min(n - 1, cur + delta))
        self.sel[self.tab_name] = cur
        return cur

    def goto(self, n, where):
        if n <= 0:
            self.sel[self.tab_name] = 0
            return 0
        self.sel[self.tab_name] = 0 if where == "first" else n - 1
        return self.sel[self.tab_name]

    def scroll_by(self, delta, maxrow):
        cur = self.scroll.get(self.tab_name, 0) + delta
        self.scroll[self.tab_name] = max(0, min(max(0, maxrow), cur))
        return self.scroll[self.tab_name]

    def say(self, msg, now=None):
        self.flash = msg
        self.flash_at = time.time() if now is None else now


class Compose:
    """The compose/reply modal. Pure — curses only feeds it keys."""

    FIELDS = ("to", "project", "subject", "body")

    def __init__(self, to="codex", subject="", thread="", project=None, frm="operator",
                 here=None):
        self.to = to if to in mail.AGENTS + (mail.EVERYONE,) else "codex"
        self.subject = subject or ""
        self.body = ""
        self.thread = thread or ""
        # Default the project to the repo we are sitting in: it is the field
        # that narrows a message to a repo, and an untagged message cannot be
        # filtered out of anyone's board.
        if project is None:
            project = Path(repo_of_path(here or os.getcwd())).name
        self.project = project or ""
        self.frm = frm
        self.field = 0

    @property
    def field_name(self):
        return self.FIELDS[self.field]

    def next_field(self, d=1):
        self.field = (self.field + d) % len(self.FIELDS)
        return self.field_name

    def cycle_to(self, d=1):
        opts = list(mail.AGENTS) + [mail.EVERYONE]
        opts = [o for o in opts if o != self.frm]
        i = opts.index(self.to) if self.to in opts else 0
        self.to = opts[(i + d) % len(opts)]
        return self.to

    def insert(self, ch):
        if self.field_name == "to":
            return
        if self.field_name == "project":
            if len(self.project) < 80:
                self.project += ch
        elif self.field_name == "subject":
            if len(self.subject) < mail.MAX_SUBJECT:
                self.subject += ch
        else:
            if len(self.body) < mail.MAX_TEXT:
                self.body += ch

    def newline(self):
        if self.field_name == "body":
            self.insert("\n")
            return True
        self.next_field()
        return False

    def backspace(self):
        if self.field_name == "project":
            self.project = self.project[:-1]
        elif self.field_name == "subject":
            self.subject = self.subject[:-1]
        elif self.field_name == "body":
            self.body = self.body[:-1]

    def ready(self):
        return bool(self.body.strip())

    def warnings(self):
        """What is worth saying before this message is sent. Advisory — the
        send is never blocked, because the operator may genuinely mean to broadcast."""
        w = []
        if self.to == mail.EVERYONE:
            w.append("📢 broadcast: `all` reaches every claude AND codex session")
        if not self.project.strip():
            w.append("no project tag — this message cannot be filtered to a repo")
        return w

    def send(self, root=None):
        """The ONE write path in this program besides ack()."""
        if not self.ready():
            raise ValueError("body is empty")
        return mail.send(self.body, self.to, frm=self.frm, subject=self.subject,
                         project=self.project, thread=self.thread, root=root)


def compose_lines(c, w):
    cur = c.field_name
    out = [L("hdr", fit(f"─ COMPOSE ({c.frm} →) " + "─" * w, w))]
    out.append(L("sel" if cur == "to" else "key",
                 f"  to      : {c.to}    " + ("← → to change" if cur == "to" else "")))
    out.append(L("sel" if cur == "project" else "key",
                 f"  project : {fit(c.project, max(4, w - 14))}" + ("▏" if cur == "project" else "")))
    out.append(L("sel" if cur == "subject" else "key",
                 f"  subject : {fit(c.subject, max(4, w - 14))}" + ("▏" if cur == "subject" else "")))
    if c.thread:
        out.append(L("dim", f"  thread  : {c.thread}"))
    for warn in c.warnings():
        out.append(L("warn", "  " + warn))
    out.append(L("sel" if cur == "body" else "key", "  body:"))
    body = c.body + ("▏" if cur == "body" else "")
    for line in wrap(body, max(8, w - 4)) or [""]:
        out.append(L("plain", "   " + line))
    out.append(L("plain", ""))
    out.append(L("dim", "  Tab field · Ctrl-S / F2 send · Esc cancel"
                        + ("" if c.ready() else "   (body is empty — nothing to send)")))
    return out


# ══════════════════════════════════════════════════════════════════════════
# CHAT — the three agents side by side, and who is in control
# ══════════════════════════════════════════════════════════════════════════
#
# the operator, 2026-09-05: *"a proper HUI chat app and terminal; I can enter text in
# each chat; he [lilJack] is setting who is in control now; slice and dice
# tiles."*  Everything on this tab comes from the agent daemon over its socket
# (`liljack_chat.call`): the tiles are `list` + `tail`, the banner is
# `control`, the give-control menu offers exactly what `list` says is running.
# When the socket is absent the tab prints `liljack_chat.START_HINT` and
# nothing else — never a blank pane, never an invented transcript.
#
# ⚠ The third-opinion tile is UNTRUSTED and its text is DATA: it goes through
# `as_data()` (control characters and escapes become `?`), it is never
# executed, never linkified, and the tile carries a footer saying so. The
# trust class comes from the daemon's `role` field via `liljack_chat.TRUST`;
# a role the table does not name renders untrusted, never trusted.

CHAT_LAYOUTS = ("side", "stack", "focus")
CHAT_TAIL_N = 50


def collect_chat(path=None, n=CHAT_TAIL_N, prev=None):
    """`list` + `control` + one `tail` per agent, in one snapshot.

    `prev` is a dict the caller keeps between calls; the collector records the
    last holder in it so the banner can draw a handover (`claude → codex`) for
    a few seconds after it happens, from a change it actually observed."""
    prev = {} if prev is None else prev
    sock = str(chat.sock_path(path))
    try:
        lst = chat.call("list", path=path)
        ctl = chat.call("control", path=path)
    except chat.AgentdAbsent:
        return {"ok": False, "error": chat.START_HINT, "path": sock, "agents": [],
                "tails": {}, "holder": None}
    except chat.AgentdError as exc:
        return {"ok": False, "error": f"agentd: {exc}", "path": sock, "agents": [],
                "tails": {}, "holder": None}
    agents = [a for a in (lst.get("agents") or []) if isinstance(a, dict) and a.get("name")]
    holder = ctl.get("holder") if isinstance(ctl, dict) else None
    tails, errors = {}, {}
    for a in agents:
        try:
            tails[a["name"]] = chat.tail_rows(chat.call("tail", path=path, agent=a["name"], n=n))
        except chat.AgentdError as exc:
            tails[a["name"]] = []
            errors[a["name"]] = str(exc)
    now = time.time()
    if prev.get("holder") != holder:
        if "holder" in prev:
            prev["previous"] = prev["holder"]
            prev["changed_at"] = now
        prev["holder"] = holder
    handover = None
    if prev.get("previous") is not None and now - (prev.get("changed_at") or 0) < 12:
        handover = {"from": prev["previous"], "to": holder, "at": prev["changed_at"]}
    return {"ok": True, "agents": agents, "holder": holder, "tails": tails,
            "errors": errors, "handover": handover, "path": sock}


def as_data(text):
    """Untrusted text as printable data: control chars, escapes and zero-width
    characters become `?`. `fit()` already does the first half; the zero-width
    set is what a homoglyph or a hidden-instruction trick uses."""
    s = "" if text is None else str(text)
    out = []
    for ch in s:
        if ch in ("​", "‌", "‍", "⁠", "﻿", "‮", "‭"):
            out.append("?")
        elif ch.isprintable() or ch == " " or ch == "\n":
            out.append(ch)
        else:
            out.append("?")
    return "".join(out)


def trust_of(agent):
    return chat.TRUST.get(str((agent or {}).get("role") or ""), "untrusted")


def unread_count(lines, seen_ts):
    """Agent output lines newer than the watermark. Only `out` counts — what
    the operator typed himself is not news to him."""
    n = 0
    for r in lines or []:
        if r.get("dir") != "out":
            continue
        if seen_ts is None or _ts_key(r.get("ts")) > _ts_key(seen_ts):
            n += 1
    return n


def _ts_key(ts):
    if isinstance(ts, (int, float)):
        return (0, float(ts), "")
    if isinstance(ts, str):
        try:
            return (0, float(ts), "")
        except Exception:
            return (1, 0.0, ts)
    return (-1, 0.0, "")


def newest_ts(lines):
    best = None
    for r in lines or []:
        if best is None or _ts_key(r.get("ts")) > _ts_key(best):
            best = r.get("ts")
    return best


def chat_tiles(cs, state):
    """The tiles, in daemon order, each with everything the renderer needs."""
    tiles = []
    for i, a in enumerate((cs or {}).get("agents") or []):
        name = a["name"]
        lines = ((cs or {}).get("tails") or {}).get(name) or []
        tiles.append({
            "name": name, "role": str(a.get("role") or "?"), "trust": trust_of(a),
            "state": str(a.get("state") or "?"), "pid": a.get("pid"),
            "backend": str(a.get("backend") or ""), "control": bool(a.get("control")),
            "lines": lines, "unread": unread_count(lines, state.chat_seen.get(name)),
            "focused": i == state.chat_focus,
            "error": ((cs or {}).get("errors") or {}).get(name),
        })
    return tiles


def chat_targets(cs, me="operator"):
    """Whom control can be GIVEN to, as the daemon describes them: the
    operator, plus every agent whose `state` is running, minus the holder.
    The daemon still gets the final word — a refusal is shown verbatim."""
    holder = (cs or {}).get("holder")
    out = []
    if me != holder:
        out.append(me)
    for a in (cs or {}).get("agents") or []:
        if a.get("state") == "running" and a["name"] != holder and a["name"] not in out:
            out.append(a["name"])
    return out


def effective_layout(mode, w, h, n):
    """The layout that actually fits. Side-by-side needs ~14 columns per tile
    and stacked ~3 rows per tile; below that the next denser layout is used,
    down to a single focused tile — so a 12×5 terminal draws something."""
    n = max(1, n)
    if mode == "side" and w // n >= 14:
        return "side"
    if mode in ("side", "stack") and h // n >= 3:
        return "stack"
    return "focus"


def tile_rects(w, h, n, mode, focus=0):
    """[(x, y, w, h)] per tile index, None for a tile the layout hides.
    In `focus` mode the last row is a strip naming the hidden tiles."""
    n = max(0, n)
    if n == 0 or w <= 0 or h <= 0:
        return [None] * n
    if mode == "side":
        cw = w // n
        return [(i * cw, 0, cw if i < n - 1 else w - i * cw, h) for i in range(n)]
    if mode == "stack":
        rh = h // n
        return [(0, i * rh, w, rh if i < n - 1 else h - i * rh) for i in range(n)]
    strip = 1 if (n > 1 and h >= 2) else 0
    rects = [None] * n
    f = max(0, min(n - 1, focus))
    rects[f] = (0, 0, w, h - strip)
    return rects


def tile_lines(tile, w, h, now=None):
    """One tile as [(style, text)] rows, exactly h of them (h>=1)."""
    now = time.time() if now is None else now
    w = max(1, w)
    untrusted = tile["trust"] == "untrusted"
    st = tile["state"]
    st_style = {"running": "ok", "starting": "warn", "blocked": "warn",
                "exited": "err", "stopped": "dim"}.get(st, "warn")
    mark = "▸" if tile["focused"] else " "
    ctl = " ★ctl" if tile["control"] else ""
    un = f" ●{tile['unread']}" if tile["unread"] else ""
    flag = " ⚠untrusted" if untrusted else ""
    title = f"{mark}{tile['name']} · {tile['role']}{flag}{ctl}{un}"
    right = f" {st}{' pid ' + str(tile['pid']) if tile.get('pid') else ''} "
    if len(title) + len(right) + 2 > w:
        right = f" {st} "                      # the state outranks the pid …
    if len(title) + len(right) + 2 > w:
        title = f"{mark}{tile['name']}{flag}{ctl}{un}"   # … the role yields before the flag …
    if len(title) + len(right) + 2 > w:
        title = fit(title, max(1, w - len(right) - 2))   # … and the title yields to the state
    fill = max(0, w - len(title) - len(right) - 2)
    hdr = fit("┌" + title + " " + "─" * fill + right, w)
    rows = [L("untrust" if untrusted else ("sel" if tile["focused"] else "hdr"), hdr)]
    if h <= 1:
        return rows
    footer = None
    if untrusted and h >= 2:
        ftxt = "└ " + chat.UNTRUSTED_FOOTER + " "
        if len(ftxt) > w:                       # 3 tiles on 120 cols is 40 each
            ftxt = "└ " + chat.UNTRUSTED_FOOTER_SHORT + " "
        footer = L("untrust", fit(ftxt, w))
    body_h = h - 1 - (1 if footer else 0)
    body = []
    if tile.get("error"):
        body.append(L("err", fit("│ " + tile["error"], w)))
    if not tile["lines"]:
        if st == "running":
            body.append(L("dim", fit("│ (no transcript lines yet)", w)))
        else:
            body.append(L("dim", fit(f"│ {st} — Enter on the input line offers to spawn", w)))
    for r in tile["lines"]:
        arrow = "▸" if r.get("dir") == "in" else "◂"
        text = as_data(r.get("text"))
        stamp = ""
        try:
            stamp = datetime.fromtimestamp(float(r.get("ts"))).strftime("%H:%M") + " "
        except Exception:
            stamp = ""
        tw = max(1, w - 3 - len(stamp))     # `│` + stamp + arrow + space
        style = ("untrust" if untrusted else "plain") if r.get("dir") == "out" else "key"
        for j, seg in enumerate(wrap(text, tw) or [""]):
            lead = f"│{stamp}{arrow} " if j == 0 else "│" + " " * (len(stamp) + 2)
            body.append(L(style, fit(lead + seg, w)))
    body = body[-body_h:] if body_h > 0 else []
    while len(body) < body_h:
        body.append(L("dim", "│"))
    rows += body
    if footer:
        rows.append(footer)
    return rows[:h]


def chat_banner(cs, state, w, now=None):
    """`CONTROL: operator` / `CONTROL: claude → codex`, styled by who holds it."""
    now = time.time() if now is None else now
    if not (cs or {}).get("ok"):
        return L("err", fit("CONTROL: ? · " + ((cs or {}).get("error") or "agentd unknown"), w))
    holder = cs.get("holder") or "?"
    ho = cs.get("handover")
    txt = f"CONTROL: {holder}"
    if ho and ho.get("to") == holder and ho.get("from") not in (None, holder):
        txt = f"CONTROL: {ho['from']} → {holder}"
    by_name = {a["name"]: a for a in cs.get("agents") or []}
    if holder == state.me:
        style = "ok"
    elif holder in by_name:
        style = "untrust" if trust_of(by_name[holder]) == "untrusted" else "key"
    else:
        style = "warn"
    hint = "  c take · g give · Tab focus · s/t/f layout"
    return L(style, fit(txt + " " * max(1, w - len(txt) - len(hint)) + hint, w)
             if w > len(txt) + len(hint) else fit(txt, w))


def chat_modal_lines(state, w):
    m = state.chat_modal
    if not m:
        return []
    out = []
    if m["kind"] == "give":
        out.append(L("hdr", fit(f"─ GIVE CONTROL (holder: {m.get('holder')}) ", w)))
        if not m["targets"]:
            out.append(L("dim", fit("  nobody eligible — no other agent is running", w)))
        for i, t in enumerate(m["targets"]):
            out.append(L("sel" if i == m["idx"] else "plain", fit(f"  {'▸' if i == m['idx'] else ' '} {t}", w)))
        if m.get("error"):
            out.append(L("err", fit("  refused: " + m["error"], w)))
        out.append(L("dim", fit("  j/k choose · Enter give · Esc cancel", w)))
    elif m["kind"] == "spawn":
        out.append(L("hdr", fit(f"─ {m['agent']} is {m.get('state', 'not running')} ", w)))
        out.append(L("plain", fit(f"  spawn {m['agent']} now? your text is kept on the input line.", w)))
        if m.get("error"):
            out.append(L("err", fit("  refused: " + m["error"], w)))
        out.append(L("dim", fit("  y / Enter spawn · n / Esc cancel", w)))
    return out


def chat_rows(cs, state, w, h, now=None):
    """The whole CHAT area as h rows of segments [(x, style, text)].

    Rows are segment lists (not one string) because a side-by-side layout puts
    three tiles on one row. `view_chat` flattens them for `--once`."""
    now = time.time() if now is None else now
    w, h = max(1, w), max(1, h)
    rows = [[] for _ in range(h)]

    def put(y, x, style, text):
        if 0 <= y < h and x < w:
            t = fit(text, w - x)
            if t:
                rows[y].append((x, style, t))

    ban = chat_banner(cs, state, w, now)
    put(0, 0, ban[0], ban[1])
    if h == 1:
        return rows
    input_y = h - 1
    if not (cs or {}).get("ok"):
        put(1, 0, "err", (cs or {}).get("error") or chat.START_HINT)
        if h > 2:
            put(2, 0, "dim", "socket: " + str((cs or {}).get("path") or chat.SOCK))
        put(input_y, 0, "dim", "(no daemon — nothing to type into)")
        return rows

    tiles = chat_tiles(cs, state)
    notice_y = h - 2 if h >= 4 else None
    tiles_h = (notice_y if notice_y is not None else input_y) - 1
    modal = chat_modal_lines(state, w)
    if modal and tiles_h > 0:
        for i, (st, txt) in enumerate(modal[:tiles_h]):
            put(1 + i, 0, st, txt)
        tiles_h -= min(len(modal), tiles_h)
        top = 1 + len(modal[:len(modal)])
    else:
        top = 1
    if tiles and tiles_h > 0:
        mode = effective_layout(state.chat_layout, w, tiles_h, len(tiles))
        rects = tile_rects(w, tiles_h, len(tiles), mode, state.chat_focus)
        for tile, rect in zip(tiles, rects):
            if rect is None:
                continue
            x, y, tw, th = rect
            for i, (st, txt) in enumerate(tile_lines(tile, tw, th, now)):
                put(top + y + i, x, st, txt)
        if mode == "focus" and len(tiles) > 1 and tiles_h >= 2:
            hidden = []
            for t in tiles:
                if not t["focused"]:
                    tag = f"{t['name']}"
                    if t["trust"] == "untrusted":
                        tag += " ⚠untrusted"
                    tag += f" [{t['state']}{', ●' + str(t['unread']) if t['unread'] else ''}]"
                    hidden.append(tag)
            put(top + tiles_h - 1, 0, "dim", "hidden: " + " · ".join(hidden) + "  (Tab to focus)")
    elif not tiles:
        put(top, 0, "warn", "agentd lists no agents")
    if notice_y is not None and state.chat_notice:
        put(notice_y, 0, "warn", state.chat_notice)
    target = tiles[state.chat_focus]["name"] if tiles and 0 <= state.chat_focus < len(tiles) else "?"
    if state.chat_typing:
        prompt = f"{state.me} → {target} ▏"
        put(input_y, 0, "sel", prompt)
        put(input_y, len(prompt), "plain", state.chat_input + "▏")
    else:
        prompt = f"{state.me} → {target} · Enter/i to type"
        put(input_y, 0, "dim", prompt + ("   [" + fit(state.chat_input, 30) + "]" if state.chat_input else ""))
    return rows


def flatten_rows(rows, w):
    """Segment rows -> [(style, text)], one string per row (for --once/tests).
    The style is the first segment's; the text is the row as it would look."""
    out = []
    for segs in rows:
        buf = [" "] * max(1, w)
        style = "plain"
        for i, (x, st, txt) in enumerate(segs):
            if i == 0:
                style = st
            for k, ch in enumerate(txt):
                if 0 <= x + k < len(buf):
                    buf[x + k] = ch
        out.append(L(style, "".join(buf).rstrip()))
    return out


def view_chat(snap, state, w=100, h=24, now=None):
    return flatten_rows(chat_rows(snap.get("chat") or {}, state, w, h, now), w)


def handle_chat_key(ch, state, snap, cache, curses, sock=None):
    """Keys on the CHAT tab. Every daemon call goes through `liljack_chat.call`
    and every refusal lands on screen as the daemon's own words."""
    cs = snap.get("chat") or {}
    tiles = chat_tiles(cs, state) if cs.get("ok") else []
    n = len(tiles)
    ENTER = (10, 13, getattr(curses, "KEY_ENTER", -2))
    BACK = (getattr(curses, "KEY_BACKSPACE", -3), 127, 8)
    UP, DOWN = getattr(curses, "KEY_UP", -4), getattr(curses, "KEY_DOWN", -5)

    def refresh():
        try:
            cache.refresh("chat", force=True)
        except Exception:
            pass

    def call(op, **kw):
        return chat.call(op, path=sock, **kw)

    # ── modal first ───────────────────────────────────────────────────────
    m = state.chat_modal
    if m is not None:
        if ch == 27:
            state.chat_modal = None
            state.say("cancelled")
            return
        if m["kind"] == "give":
            if ch in (ord("j"), DOWN):
                m["idx"] = min(len(m["targets"]) - 1, m["idx"] + 1) if m["targets"] else 0
            elif ch in (ord("k"), UP):
                m["idx"] = max(0, m["idx"] - 1)
            elif ch in ENTER and m["targets"]:
                to = m["targets"][m["idx"]]
                try:
                    r = call("give_control", **{"from": m.get("holder"), "to": to})
                    state.chat_modal = None
                    state.say(f"control → {r.get('holder', to)}")
                    refresh()
                except chat.AgentdError as exc:
                    m["error"] = str(exc)
                    state.say("give refused")
            return
        if m["kind"] == "spawn":
            if ch in (ord("y"), ord("Y")) or ch in ENTER:
                try:
                    r = call("spawn", agent=m["agent"])
                    state.chat_modal = None
                    state.say(f"spawned {m['agent']} ({r.get('state', '?')}) — Enter sends your text")
                    refresh()
                except chat.AgentdError as exc:
                    m["error"] = str(exc)
                    state.say("spawn refused")
            elif ch in (ord("n"), ord("N")):
                state.chat_modal = None
            return
        return

    # ── typing mode ───────────────────────────────────────────────────────
    if state.chat_typing:
        if ch == 27:
            state.chat_typing = False
            state.chat_input = ""
            state.say("input cancelled")
            return
        if ch == ord("\t"):
            state.chat_focus_next(n)
            _chat_mark_seen(state, tiles)
            return
        if ch in BACK:
            state.chat_input = state.chat_input[:-1]
            return
        if ch in ENTER:
            _chat_send(state, tiles, call, refresh)
            return
        if 32 <= ch < 0x110000 and len(state.chat_input) < 4000:
            try:
                state.chat_input += chr(ch)
            except Exception:
                pass
        return

    # ── command mode ──────────────────────────────────────────────────────
    if ch == ord("\t"):
        state.chat_focus_next(n)
        _chat_mark_seen(state, tiles)
        return
    if ch == curses.KEY_BTAB:
        state.chat_focus_next(n, -1)
        _chat_mark_seen(state, tiles)
        return
    if ch in (ord("s"), ord("t"), ord("f")):
        state.chat_layout = {ord("s"): "side", ord("t"): "stack", ord("f"): "focus"}[ch]
        state.say(f"layout: {state.chat_layout}")
        return
    if ch in ENTER or ch == ord("i"):
        if not cs.get("ok"):
            state.chat_notice = cs.get("error") or chat.START_HINT
            return
        if state.chat_input and ch in ENTER:
            _chat_send(state, tiles, call, refresh)
            return
        state.chat_typing = True
        state.chat_notice = ""
        _chat_mark_seen(state, tiles)
        return
    if ch == ord("c"):
        try:
            r = call("take_control", who=state.me)
            state.say(f"control: {r.get('holder', state.me)}")
            state.chat_notice = ""
            refresh()
        except chat.AgentdError as exc:
            state.chat_notice = f"take_control refused: {exc}"
            state.say("take refused")
        return
    if ch == ord("g"):
        if not cs.get("ok"):
            state.chat_notice = cs.get("error") or chat.START_HINT
            return
        state.chat_modal = {"kind": "give", "targets": chat_targets(cs, state.me), "idx": 0,
                            "holder": cs.get("holder"), "error": None}
        return
    if ch == 27:
        state.chat_notice = ""
        return


def _chat_mark_seen(state, tiles):
    if tiles and 0 <= state.chat_focus < len(tiles):
        t = tiles[state.chat_focus]
        ts = newest_ts(t["lines"])
        if ts is not None:
            state.chat_seen[t["name"]] = ts


def _chat_send(state, tiles, call, refresh):
    text = state.chat_input.strip()
    if not text:
        state.chat_typing = False
        return
    if not tiles or not (0 <= state.chat_focus < len(tiles)):
        state.chat_notice = "no tile to send to"
        return
    t = tiles[state.chat_focus]
    if t["state"] != "running":
        state.chat_modal = {"kind": "spawn", "agent": t["name"], "state": t["state"], "error": None}
        return
    try:
        # `who` per the contract, `from` as liljack_agentd.py actually reads it
        call("send", agent=t["name"], text=text, who=state.me, **{"from": state.me})
        state.chat_input = ""
        state.chat_typing = False
        state.chat_notice = ""
        state.say(f"sent to {t['name']}")
        refresh()
    except chat.AgentdError as exc:
        state.chat_notice = f"send refused: {exc}"     # shown, never swallowed
        state.say("send refused")


# ══════════════════════════════════════════════════════════════════════════
# curses front-end
# ══════════════════════════════════════════════════════════════════════════

PAIRS = [
    ("hdr", 6, -1), ("key", 4, -1), ("ok", 2, -1), ("warn", 3, -1),
    ("err", 1, -1), ("dim", 8, -1), ("sel", 0, 6), ("unread", 5, -1),
    ("untrust", 0, 5),      # black on magenta: the untrusted tile cannot pass for a trusted one
]


def _styles(curses):
    """Colour where the terminal has it, attributes where it does not."""
    attrs = {"plain": curses.A_NORMAL}
    if curses.has_colors():
        try:
            curses.use_default_colors()
        except Exception:
            pass
        for i, (name, fg, bg) in enumerate(PAIRS, start=1):
            try:
                curses.init_pair(i, fg, bg)
                a = curses.color_pair(i)
            except Exception:
                a = curses.A_NORMAL
            if name in ("hdr", "unread"):
                a |= curses.A_BOLD
            if name == "sel":
                a |= curses.A_BOLD
            attrs[name] = a
    else:                                       # mono fallback — still legible
        attrs.update({"hdr": curses.A_BOLD, "key": curses.A_BOLD,
                      "ok": curses.A_NORMAL, "warn": curses.A_BOLD,
                      "err": curses.A_BOLD | curses.A_REVERSE,
                      "dim": curses.A_DIM, "sel": curses.A_REVERSE,
                      "unread": curses.A_BOLD,
                      "untrust": curses.A_REVERSE | curses.A_UNDERLINE})
    return attrs


def _add(win, y, x, text, attr, w):
    """Clipped write. curses raises when you touch the last cell — swallow it."""
    if y < 0 or x < 0:
        return
    s = fit(text, max(0, w - x))
    if not s:
        return
    try:
        win.addstr(y, x, s, attr)
    except Exception:
        pass


def run_curses(stdscr, cache, state, root=None, sock=None):
    import curses
    curses.curs_set(0)
    if hasattr(curses, "set_escdelay"):
        try:
            curses.set_escdelay(25)
        except Exception:
            pass
    stdscr.nodelay(True)
    A = _styles(curses)
    compose = None
    stop = threading.Event()

    def collector():
        while not stop.is_set():
            cache.refresh_due()
            stop.wait(0.4)

    th = threading.Thread(target=collector, name="lj-collect", daemon=True)
    th.start()
    cache.refresh_due()

    last_draw = 0.0
    try:
        while True:
            now = time.time()
            H, W = stdscr.getmaxyx()
            snap, _ = cache.snapshot()

            if now - last_draw >= 0.2:
                stdscr.erase()
                draw(stdscr, A, snap, state, compose, W, H, now)
                stdscr.noutrefresh()
                curses.doupdate()
                last_draw = now

            ch = stdscr.getch()
            if ch == -1:
                time.sleep(0.05)
                continue
            if ch == curses.KEY_RESIZE:
                try:
                    curses.update_lines_cols()
                except Exception:
                    pass
                last_draw = 0
                continue

            if compose is not None:
                compose, done = handle_compose(ch, compose, state, curses, root)
                last_draw = 0
                if done:
                    cache.refresh("mail", force=True)
                continue

            if wants_quit(ch, state):           # never while composing (handled above)
                break
            compose = handle_key(ch, state, snap, cache, curses, root, sock=sock)
            last_draw = 0
    finally:
        stop.set()
        th.join(timeout=1.0)


def board_threads(snap, state):
    ml = snap.get("mail") or {}
    msgs = filter_by_project(ml.get("msgs") or [], state.project)
    threads = group_threads(msgs)
    if state.unread_only:
        threads = [t for t in threads if t["unread"]]
    return threads


def wants_quit(ch, state):
    """`q` / Ctrl-Q quit — except that a `q` typed INTO a chat, or pressed
    while a chat modal is open, is text / a modal key, not a quit."""
    if ch == 17:
        return True
    if ch != ord("q"):
        return False
    return not (state.tab_name == "CHAT" and (state.chat_typing or state.chat_modal is not None))


def handle_key(ch, state, snap, cache, curses, root=None, sock=None):
    """Returns a Compose to open, or None. Kept small: every branch is a state
    transition or one mailbox API call."""
    # CHAT owns the keyboard while the operator is typing or a modal is open — a `1`
    # or a `?` typed into a chat is text, not a tab jump.
    if state.tab_name == "CHAT" and (state.chat_typing or state.chat_modal is not None):
        handle_chat_key(ch, state, snap, cache, curses, sock=sock)
        return None
    if ch in (ord("?"),):
        state.help = not state.help
        return None
    if state.help and ch not in (ord("?"),):
        state.help = False
    if state.tab_name == "CHAT" and ch in (ord("\t"), ord("s"), ord("t"), ord("f"), ord("i"),
                                           ord("c"), ord("g"), 27, 10, 13, curses.KEY_ENTER):
        handle_chat_key(ch, state, snap, cache, curses, sock=sock)
        return None
    for i, _t in enumerate(TABS):
        if ch == ord(str(i + 1)):
            state.set_tab(i)
            return None
    if ch == ord("\t") or ch == curses.KEY_RIGHT:
        state.next_tab(1)
        return None
    if ch in (curses.KEY_BTAB, curses.KEY_LEFT):
        state.next_tab(-1)
        return None
    if ch == ord("R"):
        for name in list(cache.sources):
            cache.refresh(name, force=True)
        state.say("refreshed every source")
        return None

    threads = board_threads(snap, state)
    n = len(threads) if state.tab_name == "BOARD" else 0

    if ch in (ord("j"), curses.KEY_DOWN):
        if state.tab_name == "BOARD":
            state.move(n, 1)
            state.scroll[state.tab_name] = 0
        else:
            state.scroll_by(1, 10 ** 6)
        return None
    if ch in (ord("k"), curses.KEY_UP):
        if state.tab_name == "BOARD":
            state.move(n, -1)
            state.scroll[state.tab_name] = 0
        else:
            state.scroll_by(-1, 10 ** 6)
        return None
    if ch == ord("g"):
        if state.tab_name == "BOARD":
            state.goto(n, "first")
        state.scroll[state.tab_name] = 0
        return None
    if ch == ord("G"):
        if state.tab_name == "BOARD":
            state.goto(n, "last")
        return None
    if ch == curses.KEY_NPAGE:
        state.scroll_by(10, 10 ** 6)
        return None
    if ch == curses.KEY_PPAGE:
        state.scroll_by(-10, 10 ** 6)
        return None
    if ch == ord("u"):
        state.unread_only = not state.unread_only
        state.sel["BOARD"] = 0
        state.say("unread only" if state.unread_only else "all threads")
        return None
    if ch == ord("p"):
        opts = projects_of((snap.get("mail") or {}).get("msgs") or [])
        state.cycle_project(opts)
        state.sel["BOARD"] = 0
        state.say(f"project: {state.project}")
        return None

    if state.tab_name != "BOARD":
        return None

    sel = state.sel.get("BOARD", 0)
    th = threads[sel] if 0 <= sel < len(threads) else None

    if ch in (curses.KEY_ENTER, 10, 13):
        if th:
            ids = unread_for(th["msgs"], state.me)
            if ids:
                try:
                    k = mail.ack(ids, state.me, root=root)
                    state.say(f"acked {k} message(s) as {state.me}")
                    cache.refresh("mail", force=True)
                except Exception as exc:
                    state.say(f"ack failed: {type(exc).__name__}")
            else:
                state.say("nothing unread in this thread")
        return None
    if ch == ord("a"):
        allm = (snap.get("mail") or {}).get("msgs") or []
        ids = unread_for(allm, state.me)
        if ids:
            try:
                k = mail.ack(ids, state.me, root=root)
                state.say(f"acked {k} message(s) as {state.me}")
                cache.refresh("mail", force=True)
            except Exception as exc:
                state.say(f"ack failed: {type(exc).__name__}")
        else:
            state.say(f"no unread mail for {state.me}")
        return None
    if ch == ord("c"):
        proj = None if state.project in (ALL_PROJECTS, "(none)") else state.project
        return Compose(frm=state.me, project=proj)
    if ch == ord("r"):
        if not th:
            state.say("no thread selected")
            return None
        d = reply_defaults(th, state.me)
        return Compose(to=d["to"], subject=d["subject"], thread=d["thread"],
                       project=d["project"] or None, frm=state.me)
    return None


def handle_compose(ch, c, state, curses, root=None):
    """(compose|None, sent?) — Esc cancels, Ctrl-S/F2 sends."""
    if ch == 27:                                # Esc
        state.say("compose cancelled")
        return None, False
    if ch in (19, curses.KEY_F2):               # Ctrl-S / F2
        try:
            row = c.send(root=root)
            state.say(f"sent to {row['to']} · id {row['id']}")
            return None, True
        except Exception as exc:
            state.say(f"send refused: {exc}")
            return c, False
    if ch == ord("\t"):
        c.next_field(1)
        return c, False
    if ch == curses.KEY_BTAB:
        c.next_field(-1)
        return c, False
    if c.field_name == "to" and ch in (curses.KEY_LEFT, curses.KEY_RIGHT):
        c.cycle_to(1 if ch == curses.KEY_RIGHT else -1)
        return c, False
    if ch in (curses.KEY_BACKSPACE, 127, 8):
        c.backspace()
        return c, False
    if ch in (10, 13, curses.KEY_ENTER):
        c.newline()
        return c, False
    if 32 <= ch < 0x110000:
        try:
            c.insert(chr(ch))
        except Exception:
            pass
    return c, False


def draw(stdscr, A, snap, state, compose, W, H, now):
    # ── tab bar ───────────────────────────────────────────────────────────
    ml = snap.get("mail") or {}
    unread = (ml.get("unread") or {}).get(state.me, 0) if ml.get("ok") else 0
    x = 0
    _add(stdscr, 0, x, " lilJack ", A["hdr"], W)
    x += 9
    for i, t in enumerate(TABS):
        label = f" {i+1} {t} "
        if t == "BOARD" and unread:
            label = f" {i+1} {t}({unread}) "
        _add(stdscr, 0, x, label, A["sel"] if i == state.tab else A["key"], W)
        x += len(label) + 1
    _add(stdscr, 0, max(x, W - 22), datetime.now().strftime("%H:%M:%S"), A["dim"], W)
    _add(stdscr, 1, 0, "─" * W, A["hdr"], W)

    body_top, body_bot = 2, H - 2
    avail = max(1, body_bot - body_top)

    if state.help:
        _add(stdscr, body_top, 2, "keys", A["hdr"], W)
        for i, (k, d) in enumerate(HELP):
            if body_top + 2 + i >= body_bot:
                break
            _add(stdscr, body_top + 2 + i, 4, f"{k:<18} {d}", A["plain"], W)
        _add(stdscr, H - 1, 0, pad(" ? closes this help ", W), A["dim"], W)
        return

    if compose is not None:
        lines = compose_lines(compose, W - 2)
        for i, (st, txt) in enumerate(lines[:avail]):
            _add(stdscr, body_top + i, 1, txt, A.get(st, A["plain"]), W)
    elif state.tab_name == "BOARD":
        draw_board(stdscr, A, snap, state, W, body_top, body_bot)
    elif state.tab_name == "CHAT":
        rows = chat_rows(snap.get("chat") or {}, state, W, avail, now)
        for i, segs in enumerate(rows[:avail]):
            for x, st, txt in segs:
                _add(stdscr, body_top + i, x, txt, A.get(st, A["plain"]), W)
    else:
        if state.tab_name == "STATUS":
            lines = view_status(snap, W - 2, now)
        elif state.tab_name == "PEERS":
            lines = view_peers(snap, W - 2, now)
        else:
            lines = view_runs(snap, W - 2, now)
        off = min(state.scroll.get(state.tab_name, 0), max(0, len(lines) - avail))
        state.scroll[state.tab_name] = off
        for i, (st, txt) in enumerate(lines[off:off + avail]):
            _add(stdscr, body_top + i, 1, txt, A.get(st, A["plain"]), W)
        if len(lines) > avail:
            _add(stdscr, body_bot - 1, W - 12,
                 f"{off + avail}/{len(lines)}", A["dim"], W)

    # ── ribbon ────────────────────────────────────────────────────────────
    _add(stdscr, H - 2, 0, "─" * W, A["hdr"], W)
    left = ribbon(snap, W - 26, now)
    _add(stdscr, H - 1, 0, pad(left, W - 24), A["ok"], W)
    if state.flash and now - state.flash_at < 6:
        _add(stdscr, H - 1, max(0, W - 24), fit(state.flash, 23), A["warn"], W)
    else:
        _add(stdscr, H - 1, max(0, W - 24), "? keys · q quit", A["dim"], W)


def draw_board(stdscr, A, snap, state, W, top, bot):
    ml = snap.get("mail") or {}
    if not ml.get("ok"):
        _add(stdscr, top, 2, ml.get("error") or "mailbox unreadable", A["err"], W)
        return
    threads = board_threads(snap, state)
    lw = max(22, min(46, W // 3))
    sel = state.sel.get("BOARD", 0)
    if sel >= len(threads):
        sel = state.sel["BOARD"] = max(0, len(threads) - 1)

    avail = bot - top
    first = max(0, min(sel - avail // 2, max(0, len(threads) - avail)))
    head = (f"THREADS ({len(threads)}{' unread' if state.unread_only else ''}) "
            f"· {state.project}")
    _add(stdscr, top, 0, fit(head, lw), A["hdr"], W)
    for i, (st, txt) in enumerate(view_board_list(threads, sel, lw)[first:first + avail - 1]):
        _add(stdscr, top + 1 + i, 0, pad(txt, lw), A.get(st, A["plain"]), W)
    for y in range(top, bot):
        _add(stdscr, y, lw, "│", A["hdr"], W)

    th = threads[sel] if 0 <= sel < len(threads) else None
    rw = W - lw - 2
    lines = view_board_thread(th, rw, state.me)
    off = min(state.scroll.get("BOARD", 0), max(0, len(lines) - avail))
    state.scroll["BOARD"] = off
    for i, (st, txt) in enumerate(lines[off:off + avail]):
        _add(stdscr, top + i, lw + 2, txt, A.get(st, A["plain"]), W)
    if len(lines) > avail:
        _add(stdscr, bot - 1, W - 12, f"{off + avail}/{len(lines)}", A["dim"], W)


# ══════════════════════════════════════════════════════════════════════════

def dump(cache, state, width=110):
    """`--once`: the same view functions, printed. No terminal required."""
    cache.refresh_due()
    snap, ages = cache.snapshot()
    now = time.time()
    out = []
    out.append("lilJack — " + " · ".join(TABS))
    out.append("=" * width)
    for name, lines in (("STATUS", view_status(snap, width, now)),
                        ("PEERS", view_peers(snap, width, now)),
                        ("RUNS", view_runs(snap, width, now))):
        out.append("")
        out.append(f"### {name}")
        out += [t for _s, t in lines]
    out.append("")
    out.append(f"### BOARD  (project filter: {state.project}; "
               f"tags seen: {', '.join(projects_of((snap.get('mail') or {}).get('msgs') or []))})")
    threads = board_threads(snap, state)
    out += [t for _s, t in view_board_list(threads, 0, 60)]
    if threads:
        out.append("")
        out += [t for _s, t in view_board_thread(threads[0], width)]
    out.append("")
    out.append(f"### CHAT  (layout: {state.chat_layout}; focus: {state.chat_focus})")
    out += [t for _s, t in view_chat(snap, state, width, 24, now)]
    out.append("")
    out.append(ribbon(snap, width, now))
    return "\n".join(out)


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description="lilJack terminal — board + work status")
    ap.add_argument("--once", action="store_true", help="print one plain-text dump and exit")
    ap.add_argument("--tab", default="board", help="board|status|peers|runs|chat")
    ap.add_argument("--agentd-sock", default=None,
                    help="agent daemon socket (default ~/.cache/liljack/agentd.sock)")
    ap.add_argument("--demo", action="store_true",
                    help="CHAT against an in-process FAKE daemon (nothing is spawned)")
    ap.add_argument("--layout", default="side", choices=list(CHAT_LAYOUTS),
                    help="CHAT tile layout to start with")
    ap.add_argument("--project", default=ALL_PROJECTS,
                    help="board project filter (default: all)")
    ap.add_argument("--agent", default="operator", choices=list(mail.AGENTS),
                    help="who is at the keyboard (default operator)")
    ap.add_argument("--mail-root", default=None, help="mailbox dir (tests)")
    a = ap.parse_args(argv)

    try:
        tab = TABS.index(a.tab.upper())
    except ValueError:
        tab = 0
    state = State(tab=tab, me=a.agent)
    state.project = a.project
    state.chat_layout = a.layout
    root = a.mail_root
    sources = dict(SOURCES)
    if root:
        sources["mail"] = (lambda: collect_mail(root=root), 1.0)
    fake = None
    sock = a.agentd_sock
    if a.demo:
        fake = chat.demo_fake()
        sock = fake.path
    prev = {}
    sources["chat"] = (lambda: collect_chat(path=sock, prev=prev), 1.0)
    cache = Cache(sources)

    try:
        if a.once:
            print(dump(cache, state))
            return 0

        import curses
        if not sys.stdout.isatty():
            sys.stderr.write("liljack_tui: not a terminal — use --once for a plain dump\n")
            return 2
        curses.wrapper(run_curses, cache, state, root, sock)
        return 0
    finally:
        if fake is not None:
            fake.stop()


if __name__ == "__main__":
    sys.exit(main())
