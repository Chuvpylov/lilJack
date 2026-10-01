#!/usr/bin/env python3
"""
liljack_heartbeat.py — the chat watcher that keeps Codex working.

the operator, 2026-09-05: "run a heartbeat and chat watcher — when Codex finishes he
reports and awaits a new task. Uroboros style, with Claude in control."

The loop is: a task is assigned in the REGISTRY (`liljack_tasks`) → the agent's
session goes idle → this watcher takes the OLDEST `assigned` task for that
agent, pushes it verbatim into his live session with `codex queue`, and moves
the task to `active`. `done` is never set here: finishing is the agent's own
report.

⚠ THE QUEUE IS NO LONGER THE MAILBOX (fixed 2026-09-05). It used to be: an
UNREAD message whose subject began `TASK:` was the work queue. Codex then read
his mail through the MCP `read_messages` tool — which acks — so four real,
unfinished tasks became "read", this watcher reported `idle_no_task`, and from
the outside it looked like the agent had bailed. He had not; the state was
never recorded anywhere.

  ⇒ Task state is explicit and durable. Reading a message is an act of
    reading, and it consumes nothing. `TASK:` mail is still honoured, but as an
    IMPORTER: every such message becomes a registry task exactly once, read or
    unread (`liljack_tasks.import_mail`, which deliberately scans the FULL
    mailbox and dedups on message id — that is the regression fix).

⚠ THE WATCHER NEVER AUTHORS A TASK. It forwards a message body byte for byte
and prepends one envelope line naming the message id. When the queue is empty
it logs "idle, no queued task" and sleeps — Codex idling with nothing queued is
the correct resting state, not a prompt to invent work. An autonomous loop that
writes its own instructions is exactly how this goes wrong, so the only text
this program can put in front of Codex is text a human or Claude wrote into the
mailbox first.

⚠ Four stores, four rules:
  · `the trainer/data/braid/agent_tasks.bmd` — the task registry, the QUEUE. One
    BMD file under a lock, written only through `liljack_tasks`.
  · `~/.cache/liljack/mailbox*.jsonl` — chatter, and the legacy task feed. APPEND-ONLY, two
    harnesses write it with no lock, so it is only ever touched through
    `liljack_mail`'s API (`inbox`/`ack`/`send`), never rewritten.
  · `~/.codex/liljack/lifecycle.sqlite3` — Codex's own live DB. Opened
    `mode=ro` through a URI, never written, never locked.
  · `~/.cache/liljack/heartbeat.log` — ours, JSONL, append-only. It is also the
    budget ledger: the hourly cap and the minimum interval are read back out of
    it, so a restart cannot reset them by forgetting.

⚠ `SubagentStop` is NOT a turn end. It carries the PARENT session id (see
`liljack_codex.record_event`), so a session whose newest event is SubagentStop
is mid-turn with a subagent just finished. Only `Stop` / `SessionEnd` /
`Interrupt` end a turn; a newest `PermissionRequest` means BLOCKED, which is
reported and never delivered on top of.

⚠ Notices are mailed `frm="codex"` because the mailbox's `AGENTS` tuple is
`("claude","codex","operator")` and `send()` refuses claude→claude — a watcher
identity would mean editing `liljack_mail.py`, which this session may not do.
Every notice therefore carries the subject prefix `HEARTBEAT:` and opens with a
line saying it was written by this program and not by Codex. Flip `NOTICE_FROM`
if a fourth agent id ever exists.

Run it as a user unit — a backgrounded shell job dies with the shell:

  cd /path/to/your/project && \
  systemd-run --user --unit=liljack-heartbeat --collect \
    -p WorkingDirectory=$PWD -p Restart=on-failure \
    --setenv=CUDA_VISIBLE_DEVICES= \
    python3 toolbox/liljack_heartbeat.py --loop --interval 60

  systemctl --user stop liljack-heartbeat      # or: touch ~/.cache/liljack/heartbeat.stop
  journalctl --user -u liljack-heartbeat -f

Kill switch: `~/.cache/liljack/heartbeat.stop`. While that file exists nothing
is delivered, checked every tick.

Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import contextlib
import sqlite3
import subprocess
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import liljack_mail as mail                                        # noqa: E402
import liljack_sessions as sessions                                # noqa: E402
import liljack_tasks as tasks                                      # noqa: E402

CACHE = Path(os.environ.get("LILJACK_CACHE", str(Path.home() / ".cache" / "liljack")))
CODEX_DB = Path(os.environ.get("LILJACK_CODEX_DB",
                               str(Path.home() / ".codex" / "liljack" / "lifecycle.sqlite3")))

# ── the contract ──────────────────────────────────────────────────────────
TASK_PREFIX = "TASK:"          # literal, on the SUBJECT. Anything else is chat.
TURN_END = ("Stop", "SessionEnd", "Interrupt")
BLOCKED_EVENT = "PermissionRequest"
NOTICE_FROM = "codex"          # see the docstring — a constrained choice
NOTICE_MARK = "(automated notice from toolbox/liljack_heartbeat.py — not written by Codex)"

# ── budgets, all overridable from the CLI ─────────────────────────────────
SETTLE = 60                    # seconds a turn end must be old before it is idle
MIN_INTERVAL = 120             # seconds between two deliveries
MAX_PER_HOUR = 12
MAX_PER_RUN = 50
INTERVAL = 60                  # loop sleep
EVENT_LIMIT = 400              # newest lifecycle rows to consider
NOTICE_WINDOW = 6 * 3600       # do not repeat the same notice within this
SESSION_HORIZON = 24 * 3600    # a session with no event for this long is stale

# ── payloads that are the operator's call, never an automatic delivery's ─────────
# Each entry is (name, regex). A body matching ANY of them is refused: logged,
# mailed to Claude, acked so the queue advances, and never put in front of Codex.
DANGER = [
    ("systemctl", re.compile(r"\bsystemctl\b", re.I)),
    ("systemd-run", re.compile(r"\bsystemd-run\b", re.I)),
    ("git commit", re.compile(r"\bgit\s+commit\b", re.I)),
    ("git push", re.compile(r"\bgit\s+push\b", re.I)),
    # -rf, -fr, -Rf, -rvf … any rm flag cluster carrying both r and f
    ("rm -rf", re.compile(r"\brm\s+(?:-\S+\s+)*-\S*(?:r\S*f|f\S*r)", re.I)),
    # set to anything non-empty; `CUDA_VISIBLE_DEVICES=` and `=""` are fine
    ("CUDA_VISIBLE_DEVICES set", re.compile(r"""CUDA_VISIBLE_DEVICES\s*=\s*(?!(?:""|'')(?:\s|$))\S""")),
    ("trio-v5-walk-queue", re.compile(r"trio-v5-walk-queue", re.I)),
]


# ══════════════════════════════════════════════════════════════════════════
# pure helpers — no I/O, fully covered by tests
# ══════════════════════════════════════════════════════════════════════════

def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def parse_ts(s) -> datetime | None:
    """Lifecycle + mailbox timestamps are ISO-8601 UTC. A bad one is None, never a guess."""
    if not isinstance(s, str) or not s.strip():
        return None
    try:
        dt = datetime.fromisoformat(s.strip().replace("Z", "+00:00"))
    except Exception:
        return None
    return dt if dt.tzinfo else dt.replace(tzinfo=timezone.utc)


def screen(text: str) -> list:
    """Names of every dangerous pattern in `text`. Empty list = safe to deliver."""
    if not isinstance(text, str):
        return ["not-text"]
    return [name for name, rx in DANGER if rx.search(text)]


def session_states(rows, now=None, settle=SETTLE, horizon=SESSION_HORIZON) -> list:
    """Collapse lifecycle rows into one state per session, newest first.

    ⚠ ONE implementation, in `liljack_sessions`. This wrapper exists only to
    keep the LEGACY SHAPE this watcher's callers were written against: a
    `SessionEnd` row is reported as `state="idle", ended=True` rather than as
    the registry's own `state="ended"`. Both are refused by `pick_target` — the
    `ended` flag is what decides, not the word — so the two shapes cannot
    disagree about a delivery, only about a label.
    """
    out = sessions.collapse(rows, agent="codex", harness="codex", now=now,
                            settle=settle, horizon=horizon)
    for row in out:
        if row["state"] == "ended":
            row["state"] = "idle"
    return out


def pick_target(states, cwd_filter=None):
    """The idle session to wake, or None. Delegates to `liljack_sessions`.

    ⚠ An ENDED session is never a target. The first draft ranked ended sessions
    last instead of dropping them, on the theory that a failed `codex queue`
    would not be acked — but on 2026-09-05 the CLI returned SUCCESS for a thread
    whose only event was `SessionEnd`, so the task was acked and delivered
    nowhere. Measured, not theorised: rank-last is not a guard when the failure
    is silent. A COMPACTING session is refused for the same reason it is not
    idle — it is mid-turn, packing context.
    """
    return sessions.pick_target(states, cwd_filter=cwd_filter)


def is_task(msg) -> bool:
    """A `TASK:`-subject message. Kept as this module's name for it; the rule
    itself lives in `liljack_tasks.is_task_mail`, which is what the importer
    uses, so the watcher and the registry cannot disagree about what a task
    mail is."""
    return tasks.is_task_mail(msg)


def envelope(msg) -> str:
    """LEGACY: the mail-shaped envelope, one line then the body verbatim.

    Retained because it is the exact text a delivery used to carry and the log
    records show it. Live deliveries now render `liljack_tasks.envelope`, which
    produces the same shape for a mail-imported task — one identification line,
    then the brief byte for byte — plus the acceptance / do-not-touch / report
    lines when the task actually carries them.
    """
    subj = str(msg.get("subject") or "").strip()
    return (f"[lilJack heartbeat] queued task {msg.get('id','?')} from "
            f"{msg.get('frm','?')} · {subj}\n"
            f"{msg.get('text','')}")


# ══════════════════════════════════════════════════════════════════════════
# I/O — each returns a value and never raises out
# ══════════════════════════════════════════════════════════════════════════

def load_events(db_path=None, limit=EVENT_LIMIT) -> dict:
    """Newest lifecycle rows, through the registry's Codex adapter.

    ⚠ read-only URI, never written — another process owns this DB. Kept as a
    name here because the tick and the tests speak in terms of `rows`; the
    sqlite is opened in exactly one place, `liljack_sessions.codex_source`.
    """
    return sessions.codex_source(db_path=db_path, limit=limit)


def log_path(root=None) -> Path:
    return (Path(root) if root else CACHE) / "heartbeat.log"


def _no_work_left(rep, wrep=None, qrep=None):
    """Reason to retire, or None to keep running. Silence is never a reason."""
    delivery = (rep or {}).get('delivery') or {}
    if delivery.get('pending'):
        return None                     # notices still owed to the lead
    # ⚠ An unanswered question is work the room is waiting on its lead for: its
    # todo is blocked (not armed) but the question still needs a timer path to
    # escalate. Keep the loop alive so question strikes keep progressing — never
    # retire silently on unanswered questions.
    if (qrep or {}).get("open_questions"):
        return None
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent / 'liljack_app'))
        from review_panel import open_work
        work = open_work()
    except Exception:
        return None                     # cannot tell -> keep running
    if work.get('error') or work.get('open') is None:
        return None                     # cannot tell -> keep running
    if work['open'] > 0:
        return None
    # ⚠ THE BOARD IS NOT THE QUEUE. This consulted open_work() (the BOARD)
    # alone, and the board's own standing note says otherwise: "task-registry
    # = live — the trainer/data/braid/agent_tasks.bmd is the durable queue now;
    # the mailbox is chatter plus an importer."
    #
    # Measured consequence, 2026-09-09: the service exited 0/SUCCESS at
    # 01:24:30 because the board had gone all-done, and FIVE MINUTES LATER
    # five trio tasks were opened in the REGISTRY. Nothing re-armed it, so
    # three agents sat idle against a full queue and the operator had to notice by
    # hand, twice. Retiring on an empty board while the durable queue is full
    # is the service concluding it has nothing to deliver on the strength of
    # the one list it does not deliver from.
    #
    # the operator's rule, 2026-09-09: the heartbeat spanks everyone until the work
    # is actually done. An open REGISTRY task therefore keeps it alive, and
    # "cannot tell" keeps it alive too — silence is never a reason to retire.
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import liljack_tasks
        open_tasks = liljack_tasks.tasks(open_only=True)
    except Exception:
        return None                     # cannot tell -> keep running
    # ⚠ BLOCKED IS NOT PENDING (see liljack_tasks.PENDING). A pending task
    # keeps the heartbeat alive — it is actionable. A blocked task is present
    # but nobody can act on it, so it neither arms the loop nor keeps it
    # alive on its own; a room whose work is all blocked must not tick
    # forever with nothing to deliver. The blocked ids are NAMED here so the
    # rest is a decision, never a silent disappearance.
    pending = [t for t in open_tasks if t["state"] in tasks.PENDING]
    if pending:
        return None                     # the durable queue still has actionable work
    blocked = [t for t in open_tasks if t["state"] == "blocked"]
    if blocked:
        ids = ", ".join(t["id"] for t in blocked[:5])
        more = f" (+{len(blocked) - 5})" if len(blocked) > 5 else ""
        return (f"{len(blocked)} BLOCKED registry task(s) remain ({ids}{more}) — not "
                "actionable, so the heartbeat rests; they stay on the board and "
                "re-arm if unblocked")
    return 'no open board work, no open registry task, and no pending notices'


# ── the loop must run the code that is on disk ──────────────────────────────
# ⚠ A LONG-LIVED LOOP RUNS THE MODULES IT IMPORTED AT START, FOREVER. Measured
# 2026-09-10: DeepSeek landed the fix that unblocks the whole room at 00:14
# while this service had been running since 00:00, so for fourteen minutes it
# executed the old module and nothing on the machine could tell — Codex reported
# it exactly right as "running-service code version not verified". The heartbeat
# is the one process everything else depends on, so it re-executes itself the
# moment its own source changes rather than waiting for a human to restart it.
CODE_SETTLE_S = 5.0          # let an editor finish writing before acting


def code_stamps() -> dict:
    """mtime of every module actually loaded from this toolbox.

    Derived from sys.modules rather than a hardcoded list, so a module that
    joins the import graph later is watched without anyone remembering to add
    it — which is the failure mode a list would reproduce.
    """
    root = str(Path(__file__).resolve().parent)
    out = {}
    for mod in list(sys.modules.values()):
        f = getattr(mod, "__file__", None) or ""
        if f.startswith(root) and f.endswith(".py"):
            try:
                out[f] = os.stat(f).st_mtime
            except OSError:
                pass
    return out


def code_changed(baseline: dict, now=None, settle: float = CODE_SETTLE_S):
    """The first changed file, or None. A file mid-write is NOT yet a change.

    ⚠ MODULES ARRIVE LATE AND MUST BE ADOPTED, NOT REPORTED. worker_wake and
    room_protocol are imported INSIDE the loop, so they are absent from a
    baseline taken before the first tick; treating their arrival as a change
    would re-exec on the first iteration, forever. A file seen for the first
    time is recorded at its current mtime and watched from then on.
    """
    now = now if now is not None else time.time()
    for f, mtime in code_stamps().items():
        if f not in baseline:
            baseline[f] = mtime           # newly imported: adopt, do not fire
    for f, was in list(baseline.items()):
        try:
            cur = os.stat(f).st_mtime
        except OSError:
            return f                      # deleted or replaced counts as changed
        if cur != was and (now - cur) >= settle:
            return f
    return None


def reload_self(changed: str) -> None:
    """Re-exec with the same arguments so the new code runs immediately.

    ⚠ exec, not exit. Exiting would leave the gap until the arm timer's next
    minute, and this loop is what wakes everybody — a minute of silence is the
    thing we are trying to stop. If the new code is broken the process dies and
    systemd's Restart=on-failure makes that loudly visible, which is the right
    outcome: a heartbeat that cannot start must not look like a heartbeat at
    rest.
    """
    print(json.dumps({"action": "reload", "detail": "source changed on disk",
                      "file": os.path.basename(changed)}, ensure_ascii=False), flush=True)
    sys.stdout.flush()
    os.execv(sys.executable, [sys.executable] + sys.argv)


def stop_path(root=None) -> Path:
    return (Path(root) if root else CACHE) / "heartbeat.stop"


def _pending_notices(workspace_root=None) -> int:
    """Undelivered room notices, globally. A pending notice IS work: the lead
    has not read it, and only a running heartbeat will deliver it. Cheap
    read-only count across the whole workspace store."""
    base = Path(workspace_root or os.environ.get(
        "LILJACK_WORKSPACE_ROOT", str(Path.home() / ".cache/liljack/workspace")))
    db = base / "workspace.sqlite3"
    if not db.exists():
        return 0
    try:
        with contextlib.closing(sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)) as con:
            return con.execute("SELECT COUNT(*) FROM room_delivery WHERE state='pending'").fetchone()[0]
    except sqlite3.Error:
        return 0


def arm_decision(root=None, tasks_file=None, workspace_root=None) -> dict:
    """The ARMING predicate — the symmetric inverse of `_no_work_left`.

    True when there is ≥1 actionable todo: a registry task in PENDING state
    (proposed/assigned/active) OR a pending room notice. `blocked` is reported
    but does NOT arm (see liljack_tasks.PENDING): arming on blocked work would
    start a loop with nothing to deliver and spam the owner.

    This is the "cheap and always-on" check a systemd timer runs; the heartbeat
    itself keeps its self-retire. Unknown is never rest: an unreadable registry
    is reported as an error and does not arm, which a caller must treat as
    "cannot tell", not as "no work".
    """
    pending = blocked = 0
    err = None
    released = []
    repaired = []
    try:
        # ⚠ RELEASE BEFORE COUNTING. complete() frees its waiters, but only when
        # something is running to do it — and a dependency finished while this
        # loop was down (or executing a module it loaded minutes earlier) leaves
        # its waiters blocked with nothing to wake them. the operator, 2026-09-10: "why
        # ds finished and no todos are unbloked no heartbeat spanking agents?".
        # Reconciling here means the ARM decision sees the work that just became
        # actionable, instead of resting beside it.
        # ⚠ REPAIR BEFORE RELEASING. A stale writer can still delete `room` and
        # `blocked_on` from every row (2026-09-10), and a dependency that has
        # been erased cannot release anything — the reconcile below would see a
        # blocked task waiting on nothing and leave it blocked forever. The log
        # survives that clobber, so the fields are restored from it first.
        repaired = tasks.reconcile_scopes(path=tasks_file)
        released = tasks.reconcile_blocked(path=tasks_file)
        rows = tasks.tasks(open_only=True, path=tasks_file)
        pending = sum(1 for t in rows if t["state"] in tasks.PENDING)
        blocked = sum(1 for t in rows if t["state"] == "blocked")
    except Exception as exc:
        err = f"{type(exc).__name__}: {exc}"
    notices = _pending_notices(workspace_root)
    arm = bool(pending or notices)
    if repaired:
        reason = (f"repaired {len(repaired)} clobbered field(s) from the log: {', '.join(repaired[:4])}"
                  + (" …" if len(repaired) > 4 else ""))
        if released:
            reason += f"; released {', '.join(released)}"
    elif released:
        reason = f"released {len(released)} task(s) whose dependency is done: {', '.join(released)}"
    elif err:
        reason = f"registry unreadable ({err}) — cannot tell, so not arming"
    elif arm:
        parts = []
        if pending:
            parts.append(f"{pending} pending task(s)")
        if notices:
            parts.append(f"{notices} pending notice(s)")
        reason = "arm: " + ", ".join(parts)
    elif blocked:
        reason = (f"rest: only {blocked} blocked task(s) remain — not actionable; "
                  "they stay on the board and re-arm if unblocked")
    else:
        reason = "rest: no pending tasks and no pending notices"
    return {"arm": arm, "pending_tasks": pending, "blocked_tasks": blocked,
            "pending_notices": notices, "error": err, "reason": reason}


def start_service(name: str, timeout: int = 10) -> dict:
    """Ask systemd to start a user unit. Used by --arm --start; never in a test."""
    try:
        p = subprocess.run(["systemctl", "--user", "start", "--no-block", name],
                           capture_output=True, text=True, timeout=timeout)
        return {"ok": p.returncode == 0, "rc": p.returncode,
                "err": (p.stderr or "")[-400:].strip()}
    except Exception as exc:
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}"}


def tasks_path(root=None):
    """Where the registry lives for this watcher.

    ⚠ When a `root` is given (the tests always give one) the registry lives
    INSIDE it, beside that root's mailbox and log. Without this, a test tick
    would write into the real `the trainer/data/braid/agent_tasks.bmd` — a suite
    that mutates production state is worse than no suite.
    """
    return None if root is None else Path(root) / "agent_tasks.bmd"


def log_event(row: dict, root=None) -> dict:
    """Append one JSONL record. Same O_APPEND discipline as the mailbox."""
    row = {"ts": utcnow().isoformat(), **row}
    p = log_path(root)
    p.parent.mkdir(parents=True, exist_ok=True)
    line = json.dumps(row, ensure_ascii=False) + "\n"
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.write(fd, line.encode("utf-8"))
    finally:
        os.close(fd)
    return row


def read_log(root=None, limit=2000) -> list:
    p = log_path(root)
    if not p.exists():
        return []
    out = []
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines()[-limit:]:
        line = line.strip()
        if not line:
            continue
        try:
            row = json.loads(line)
        except Exception:
            continue                 # a torn line is skipped, never fatal
        if isinstance(row, dict):
            out.append(row)
    return out


def deliveries(root=None, since=None) -> list:
    rows = [r for r in read_log(root) if r.get("event") == "delivered"]
    if since is None:
        return rows
    return [r for r in rows if (parse_ts(r.get("ts")) or utcnow()) >= since]


def notice_sent(key, root=None, now=None, window=NOTICE_WINDOW) -> bool:
    """Has this exact notice already gone out recently? Keeps the board quiet."""
    now = now or utcnow()
    cut = now - timedelta(seconds=window)
    for r in read_log(root):
        if r.get("event") == "notice" and r.get("key") == key:
            ts = parse_ts(r.get("ts"))
            if ts and ts >= cut:
                return True
    return False


def notify(subject, body, key, root=None, now=None, send_mail=None, window=NOTICE_WINDOW) -> bool:
    """Mail Claude about something a human should see. Deduped by `key`."""
    if notice_sent(key, root=root, now=now, window=window):
        return False
    fn = send_mail or mail.send
    try:
        fn(f"{NOTICE_MARK}\n{body}", "claude", frm=NOTICE_FROM,
           subject=f"HEARTBEAT: {subject}"[:mail.MAX_SUBJECT],
           project="demo", root=root)
        ok, err = True, ""
    except Exception as exc:
        ok, err = False, f"{type(exc).__name__}: {exc}"
    log_event({"event": "notice", "key": key, "subject": subject,
               "mailed": ok, "error": err}, root=root)
    return ok


def codex_queue(session: str, text: str, timeout: int = 30) -> dict:
    """The delivery mechanism. Injected in tests — never called by the suite."""
    env = {**os.environ, "CUDA_VISIBLE_DEVICES": ""}
    try:
        p = subprocess.run(["codex", "queue", "--thread", session, "--message", text],
                           capture_output=True, text=True, timeout=timeout, env=env)
    except Exception as exc:
        return {"ok": False, "error": f"{type(exc).__name__}: {exc}"}
    return {"ok": p.returncode == 0, "rc": p.returncode,
            "err": (p.stderr or "")[-400:].strip()}


# ══════════════════════════════════════════════════════════════════════════
# the tick
# ══════════════════════════════════════════════════════════════════════════

def tick(root=None, db_path=None, now=None, settle=SETTLE, cwd_filter=None,
         min_interval=MIN_INTERVAL, max_per_hour=MAX_PER_HOUR,
         max_per_run=MAX_PER_RUN, run_count=0, dry_run=True,
         deliver=None, send_mail=None, events=None, ack_refused=True,
         agent="codex", tasks_file=None) -> dict:
    """One pass. Returns a report dict; `action` says what happened.

    action ∈ stopped · no-lifecycle · no-idle-session · capped · idle-no-task ·
             refused · would-deliver · delivered · deliver-failed
    """
    now = now or utcnow()
    agent = (agent or "codex").strip().lower()
    tp = tasks_file if tasks_file is not None else tasks_path(root)
    rep = {"ts": now.isoformat(), "action": "", "detail": "", "dry_run": bool(dry_run),
           "agent": agent}

    # 1 ── kill switch, checked before anything else can act
    if stop_path(root).exists():
        rep.update(action="stopped", detail=f"kill switch present: {stop_path(root)}")
        log_event({"event": "stopped", "path": str(stop_path(root))}, root=root)
        return rep

    # 2 ── who is the agent, right now?
    ev = {"ok": True, "rows": events} if events is not None else load_events(db_path)
    if not ev["ok"]:
        rep.update(action="no-lifecycle", detail=ev.get("error", "lifecycle unavailable"))
        log_event({"event": "no_lifecycle", "error": rep["detail"]}, root=root)
        return rep
    states = session_states(ev["rows"], now=now, settle=settle)
    # ⚠ `ended` is reported alongside `state`. The legacy shape labels a
    # SessionEnd row `idle`, which reads as "deliverable" to a human scanning
    # this report — and a dead thread that looks deliverable is precisely the
    # failure that lost a task on 2026-09-05.
    rep["sessions"] = [{k: s[k] for k in ("session", "state", "ended", "event", "age", "cwd")}
                       for s in states[:6]]

    # a blocked session is reported and never delivered on top of
    for s in states:
        if s["state"] == "blocked":
            notify("Codex is blocked on a permission prompt",
                   f"session {s['session']} in {s['cwd']} has been waiting on a "
                   f"{BLOCKED_EVENT} for {int(s['age'] or 0)}s. Nothing was delivered.",
                   key=f"blocked:{s['session']}:{s['seq']}",
                   root=root, now=now, send_mail=send_mail)
            log_event({"event": "blocked", "session": s["session"], "seq": s["seq"],
                       "age": s["age"]}, root=root)
            rep["blocked"] = s["session"]

    target = pick_target(states, cwd_filter=cwd_filter)

    # 3 ── the mailbox is an IMPORTER now, not the queue.
    # ⚠ `import_mail` scans the FULL mailbox, read and unread, and dedups on
    # message id. That is the whole fix: a task Codex had already opened
    # through `read_messages` used to vanish from an unread-only queue, which is
    # how four live tasks reported `idle_no_task`. Reading consumes nothing.
    try:
        made = tasks.import_mail(agent, root=root, path=tp)
        if made:
            log_event({"event": "imported", "tasks": [t["id"] for t in made],
                       "mail": [t["mail"] for t in made]}, root=root)
            rep["imported"] = [t["id"] for t in made]
    except Exception as exc:
        log_event({"event": "import_error", "error": f"{type(exc).__name__}: {exc}"}, root=root)

    # 4 ── the queue, always reported even when we cannot act on it
    try:
        queued = [t for t in tasks.tasks(owner=agent, state=tasks.DELIVERABLE, path=tp)]
    except Exception as exc:
        queued = []
        log_event({"event": "registry_error", "error": f"{type(exc).__name__}: {exc}"}, root=root)
    rep["queue_depth"] = len(queued)
    try:
        rep["chat_waiting"] = len([m for m in mail.inbox(agent, unread_only=True, limit=200,
                                                         root=root) if not is_task(m)])
    except Exception:
        rep["chat_waiting"] = 0

    if target is None:
        rep.update(action="no-idle-session",
                   detail="no settled idle Codex session" +
                          (f" in {cwd_filter}" if cwd_filter else ""))
        return rep
    rep["target"] = target["session"]

    # 5 ── budgets, before a task is consumed
    last = deliveries(root)
    if last:
        last_ts = parse_ts(last[-1].get("ts"))
        if last_ts and (now - last_ts).total_seconds() < min_interval:
            rep.update(action="capped", detail=(
                f"min interval {min_interval}s not elapsed "
                f"({int((now - last_ts).total_seconds())}s since last delivery)"))
            log_event({"event": "cap", "cap": "min_interval"}, root=root)
            return rep
        # ⚠ do not stack a second task on a session that has not moved since the
        # last one — its Stop is still the newest event until it wakes.
        prev = [r for r in last if r.get("session") == target["session"]]
        if prev and (prev[-1].get("seq") or 0) >= target["seq"]:
            rep.update(action="capped", detail=(
                f"session {target['session']} has not advanced since delivery at "
                f"seq {prev[-1].get('seq')}"))
            log_event({"event": "cap", "cap": "no_advance",
                       "session": target["session"], "seq": target["seq"]}, root=root)
            return rep
    if run_count >= max_per_run:
        rep.update(action="capped", detail=f"max {max_per_run} deliveries per run reached")
        log_event({"event": "cap", "cap": "per_run", "count": run_count}, root=root)
        notify("delivery cap reached (per run)",
               f"{run_count} deliveries this run; the watcher has stopped delivering.",
               key=f"cap:run:{now.strftime('%Y%m%dT%H')}", root=root, now=now, send_mail=send_mail)
        return rep
    hour = len(deliveries(root, since=now - timedelta(hours=1)))
    if hour >= max_per_hour:
        rep.update(action="capped", detail=f"max {max_per_hour} deliveries/hour reached ({hour})")
        log_event({"event": "cap", "cap": "per_hour", "count": hour}, root=root)
        notify("delivery cap reached (per hour)",
               f"{hour} deliveries in the last hour; the watcher has stopped delivering.",
               key=f"cap:hour:{now.strftime('%Y%m%dT%H')}", root=root, now=now, send_mail=send_mail)
        return rep

    # 6 ── the task. No task is the resting state, not a reason to invent one.
    # ⚠ ONLY an `assigned` task is deliverable (`tasks.next_for`). An `active`
    # one is already in front of somebody and a `proposed` one has not been
    # decided — handing either out is how a queue starts authoring work.
    # ⚠ ONE definition of "the oldest assigned task", and it is `next_for`.
    # `queued` above is a DEPTH, not an ordering — picking `queued[0]` would be a
    # second ranking that agrees today and drifts the first time either sort
    # changes. This repo has paid for that twice already (status vs analysis.json,
    # and the third rank-key copy in ladder.py).
    task = tasks.next_for(agent, path=tp) if queued else None
    if task is None:
        rep.update(action="idle-no-task", detail="idle, no queued task")
        log_event({"event": "idle_no_task", "session": target["session"],
                   "agent": agent, "chat_waiting": rep["chat_waiting"]}, root=root)
        return rep
    rep["task"] = task["id"]
    rep["message"] = task.get("mail") or None      # the mail this came from, if any

    # 7 ── refuse dangerous payloads
    hits = screen(task.get("brief") or "")
    if hits:
        rep.update(action="refused", detail=f"payload matches: {', '.join(hits)}")
        log_event({"event": "refused", "task": task["id"], "message": rep["message"],
                   "patterns": hits, "subject": task.get("title"),
                   "acked": bool(ack_refused)}, root=root)
        notify(f"refused task {rep['message'] or task['id']}",
               f"subject: {task.get('title')}\nmatched: {', '.join(hits)}\n"
               "Those calls are the operator's, never an automatic delivery's. The task was "
               "NOT delivered; it is parked as BLOCKED in the registry" +
               (" and its mail was acked." if ack_refused else "."),
               key=f"refused:{task['id']}", root=root, now=now, send_mail=send_mail)
        if not dry_run:
            # ⚠ blocked, never deleted. A refused task that disappeared is the
            # same failure mode as the acked message that started all this.
            try:
                tasks.refuse(task["id"], f"heartbeat refused: {', '.join(hits)}", path=tp)
            except Exception as exc:
                log_event({"event": "refuse_error", "task": task["id"],
                           "error": f"{type(exc).__name__}: {exc}"}, root=root)
            if ack_refused and task.get("mail"):
                try:
                    mail.ack([task["mail"]], agent, root=root)
                except Exception:
                    pass
        return rep

    # 8 ── deliver, verbatim
    text = tasks.envelope(task)
    if dry_run:
        rep.update(action="would-deliver",
                   detail=f"would queue {task['id']} → {target['session']}",
                   payload=text)
        return rep
    res = (deliver or codex_queue)(target["session"], text)
    if not res.get("ok"):
        rep.update(action="deliver-failed", detail=str(res.get("error") or res.get("err") or res))
        log_event({"event": "deliver_failed", "task": task["id"], "message": rep["message"],
                   "session": target["session"], "error": rep["detail"]}, root=root)
        notify(f"delivery failed for {task['id']}",
               f"codex queue → {target['session']} failed: {rep['detail']}. "
               "The task stays ASSIGNED and will be offered again.",
               key=f"failed:{task['id']}:{target['seq']}", root=root, now=now, send_mail=send_mail)
        return rep
    # assigned → active, stamped with the session it went to. `done` is the
    # agent's own report and is never set here.
    try:
        tasks.deliver(task["id"], session=target["session"], path=tp)
        moved = True
    except Exception as exc:
        moved = False
        log_event({"event": "deliver_state_error", "task": task["id"],
                   "error": f"{type(exc).__name__}: {exc}"}, root=root)
    acked = False
    if task.get("mail"):
        try:
            mail.ack([task["mail"]], agent, root=root)
            acked = True
        except Exception:
            acked = False
    rep.update(action="delivered",
               detail=f"queued {task['id']} → {target['session']}", payload=text)
    log_event({"event": "delivered", "task": task["id"], "message": rep["message"],
               "session": target["session"], "seq": target["seq"],
               "subject": task.get("title"), "state": "active" if moved else "assigned",
               "bytes": len(text), "acked": acked}, root=root)
    return rep


def summarise(rep: dict) -> str:
    icon = {"delivered": "→", "would-deliver": "·", "refused": "✗", "capped": "⏸",
            "stopped": "⏹", "idle-no-task": "…", "no-idle-session": "…",
            "deliver-failed": "✗", "no-lifecycle": "✗"}.get(rep.get("action"), " ")
    line = f"{icon} {rep.get('action')}: {rep.get('detail')}"
    extra = []
    if "queue_depth" in rep:
        extra.append(f"queue {rep['queue_depth']}")
    if rep.get("task"):
        extra.append(f"task {rep['task']}")
    if rep.get("target"):
        extra.append(f"target {str(rep['target'])[:8]}")
    if rep.get("blocked"):
        extra.append(f"blocked {str(rep['blocked'])[:8]}")
    return line + (f"   [{' · '.join(extra)}]" if extra else "")


def room_tick(project, workspace_root=None, *, root=None, dry_run=True,
              tmux=None, lookup=None, send=None):
    """One room delivery tick, independent of any frontend or legacy task queue."""
    if stop_path(root).exists():
        return {'action': 'stopped', 'detail': str(stop_path(root))}
    from liljack_workspace import Workspace, project_key
    from liljack_app.runtime import Tmux
    from liljack_app.room_delivery import pump
    workspace_root = Path(workspace_root or os.environ.get(
        'LILJACK_WORKSPACE_ROOT', str(Path.home() / '.cache/liljack/workspace'))).expanduser().resolve()
    path = workspace_root / 'workspace.sqlite3'
    if not path.exists():
        return {'action': 'no-workspace', 'detail': 'No existing room workspace'}
    if dry_run:
        # Read-only inspection must not consume a notice or create a store.
        with contextlib.closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)) as db:
            rows = db.execute('SELECT d.state,COUNT(*) FROM room_delivery d JOIN messages m '
                              'ON m.id=d.message_id WHERE m.project=? GROUP BY d.state',
                              (project_key(project),)).fetchall()
        return {'action': 'room-dry-run', 'detail': 'No wake attempted', 'delivery': dict(rows)}
    tmux = tmux or Tmux()
    if not tmux.binary:
        return {'action': 'no-transport', 'detail': 'tmux is unavailable'}
    live = tmux.sessions(project)  # Discovery failure must propagate, never become [].
    store = Workspace(workspace_root)
    try:
        store.reconcile_terminals(project, tmux.socket, (r['id'] for r in live), actor='operator')
        for row in live:
            store.observe(row)
            store.bind_terminal(row['id'], tmux.socket, row['tmux_name'], row['pane'], actor='operator')
        pump(store, project, live, lookup=lookup, send=send)
        rows = store.db.execute('SELECT d.state,COUNT(*) FROM room_delivery d JOIN messages m '
                                'ON m.id=d.message_id WHERE m.project=? GROUP BY d.state',
                                (project_key(project),)).fetchall()
        return {'action': 'room-tick', 'detail': 'Room delivery checked', 'delivery': dict(rows)}
    finally:
        store.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description="lilJack heartbeat — wake Codex with queued tasks")
    ap.add_argument("--once", action="store_true", help="one tick, print, exit (implies --dry-run unless --live)")
    ap.add_argument("--loop", action="store_true", help="run forever, delivering for real")
    ap.add_argument("--live", action="store_true", help="with --once: actually deliver")
    ap.add_argument("--dry-run", action="store_true", help="never call `codex queue`")
    ap.add_argument("--interval", type=int, default=INTERVAL)
    ap.add_argument("--settle", type=int, default=SETTLE)
    ap.add_argument("--min-interval", type=int, default=MIN_INTERVAL)
    ap.add_argument("--max-per-hour", type=int, default=MAX_PER_HOUR)
    ap.add_argument("--max-per-run", type=int, default=MAX_PER_RUN)
    ap.add_argument("--cwd", default=str(PROJECT_ROOT), help="only wake sessions in this cwd")
    ap.add_argument("--any-cwd", action="store_true", help="wake a Codex session in any repo")
    ap.add_argument("--root", default=None, help="mailbox/log root (default ~/.cache/liljack)")
    ap.add_argument("--agent", default="codex", help="which agent's queue to serve")
    ap.add_argument("--tasks", default=None, help="task registry path (default: the shared one)")
    ap.add_argument("--db", default=None, help="lifecycle sqlite path")
    ap.add_argument("--json", action="store_true", help="print the report dict")
    ap.add_argument("--rooms", action="store_true", help="serve room lead notifications, independent of the UI")
    ap.add_argument("--workspace-root", help="room workspace root (separate from mailbox/log root)")
    ap.add_argument("--arm", action="store_true",
                    help="arming check only: print arm_decision and exit")
    ap.add_argument("--start", action="store_true",
                    help="with --arm: systemctl --user start --service when the decision is to arm")
    ap.add_argument("--service", default="liljack-room-heartbeat",
                    help="systemd --user unit to start when --arm --start decides to arm")
    a = ap.parse_args(argv)

    if a.arm:
        dec = arm_decision(root=a.root, tasks_file=a.tasks, workspace_root=a.workspace_root)
        if a.start:
            dec["start"] = start_service(a.service) if dec["arm"] else {"ok": True, "skipped": "nothing to arm"}
        print(json.dumps(dec, ensure_ascii=False), flush=True)
        return 0

    if not a.once and not a.loop:
        a.once = True                                  # safest default
    dry = a.dry_run or (a.once and not a.live)         # dry-run OFF only in a deliberate loop
    if a.rooms:
        if a.any_cwd:
            ap.error('--rooms requires one explicit --cwd project')
        stamps = code_stamps()
        while True:
            changed = code_changed(stamps)
            if changed:
                reload_self(changed)
            try:
                rep = room_tick(a.cwd, a.workspace_root, root=a.root, dry_run=dry)
                print(json.dumps(rep, ensure_ascii=False), flush=True)
            except Exception as exc:
                print(json.dumps({'action': 'room-error', 'detail': type(exc).__name__}), flush=True)
                if a.once:
                    return 1
            if a.once:
                return 0
            # RULE (the operator): the heartbeat exists to wake people about WORK. With
            # no open work it is a service delivering nothing, so it retires
            # itself rather than ticking forever.
            # ⚠ UNKNOWN IS NEVER DONE: an unreadable board, or notices still
            # queued, both mean KEEP RUNNING. Stopping early strands the team
            # with nothing able to wake them; staying up one cycle too long
            # costs a few local sqlite reads.
            # Wake idle WORKERS too, not just the lead. Without this the lead is
            # the only thing that can start anyone, so every agent stalls after
            # each turn and the whole team waits on one session.
            try:
                sys.path.insert(0, str(Path(__file__).resolve().parent / 'liljack_app'))
                from worker_wake import tick as _worker_tick, question_tick as _question_tick
                # ⚠ root here is the WORKSPACE root (worker_wake resolves the
                # sessions store under it), NOT the mailbox/log root.
                wrep = _worker_tick(a.cwd, root=a.workspace_root, dry_run=dry)
                if wrep.get('action') != 'worker-idle-none':
                    print(json.dumps(wrep, ensure_ascii=False), flush=True)
                qrep = _question_tick(a.cwd, a.workspace_root, dry_run=dry)
                if qrep.get('results'):
                    print(json.dumps({'action': 'question-tick', **qrep}, ensure_ascii=False), flush=True)
            except Exception as exc:
                print(json.dumps({'action': 'worker-error', 'detail': type(exc).__name__}), flush=True)
            done = _no_work_left(rep, wrep, qrep)
            if done:
                print(json.dumps({'action': 'retired', 'detail': done}, ensure_ascii=False), flush=True)
                return 0
            time.sleep(max(5, a.interval))
    kw = dict(root=a.root, db_path=a.db, settle=a.settle, min_interval=a.min_interval,
              max_per_hour=a.max_per_hour, max_per_run=a.max_per_run,
              cwd_filter=None if a.any_cwd else a.cwd, dry_run=dry,
              agent=a.agent, tasks_file=a.tasks)

    if a.once:
        rep = tick(**kw)
        print(json.dumps(rep, ensure_ascii=False, indent=2) if a.json else summarise(rep))
        return 0

    print(f"heartbeat: interval {a.interval}s · settle {a.settle}s · "
          f"caps {a.min_interval}s/{a.max_per_hour}h/{a.max_per_run}run · "
          f"dry_run={dry} · cwd={'any' if a.any_cwd else a.cwd}", flush=True)
    delivered = 0
    stamps = code_stamps()
    while True:
        changed = code_changed(stamps)
        if changed:
            reload_self(changed)
        try:
            rep = tick(run_count=delivered, **kw)
            if rep.get("action") == "delivered":
                delivered += 1
            print(summarise(rep), flush=True)
        except Exception as exc:                       # a tick must never kill the loop
            print(f"✗ tick raised: {type(exc).__name__}: {exc}", flush=True)
            try:
                log_event({"event": "tick_error", "error": f"{type(exc).__name__}: {exc}"},
                          root=a.root)
            except Exception:
                pass
        time.sleep(max(5, a.interval))


if __name__ == "__main__":
    sys.exit(main())
