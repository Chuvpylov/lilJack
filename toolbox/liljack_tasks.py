#!/usr/bin/env python3
"""
liljack_tasks.py — the multi-agent TASK REGISTRY, held as BMD claims.

the operator, 2026-09-05: "organise the bi-agent harness in lilJack properly — I'll add
another agent. I decide who does what and the specifics; for now 2 agents, in
future more."

⚠ THE DEFECT THIS EXISTS TO FIX (measured 2026-09-05, not hypothetical).
Task assignment used to ride on the MAILBOX: `liljack_heartbeat` treated an
*unread* message whose subject began `TASK:` as the work queue. Codex then read
his mail through the MCP `read_messages` tool — which acks — so four real,
unfinished tasks became "read", the queue reported `idle_no_task`, and from the
outside it looked like the agent had bailed. He had not; the state was never
recorded anywhere.

  ⇒ Task state is EXPLICIT and DURABLE. It is never inferred from whether
    somebody read a message. Reading mail is an act of reading, not an act of
    finishing work, and nothing in this module is driven by an ack.

⚠ FORMAT — the same three traps `liljack_board` documents, paid for again here:
  1. Claims live in the DOCUMENT-LEVEL `[[footer]]`. `bmd.parse_assertion` runs
     on footer lines only; a claim written as prose inside a pasem yields ZERO
     assertions. The pasems here are the generated summary and the per-task
     BRIEFS — prose, never claims.
  2. `fact` takes (pred, subj[, obj]) — at most TWO args after the predicate.
     A task claim is therefore `fact(task, <id>, <state> | …attrs…)`: everything
     else is an attribute, inside the parens.
  3. Any value carrying `|`, `,`, `'`, `"` or a bracket goes through
     `liljack_board.qattr()`. Predicate ARGUMENTS cannot be quoted at all
     (`parse_assertion` never unquotes them), so ids and states are slugged.

⚠ NO NEGATION-AS-FAILURE. An unassigned task is not "proved unassigned" by the
absence of an `owner=` attribute, so taking work away from an agent writes an
explicit `neg(task-owner, <id>, <agent> | reason=…)` — the same discipline the
board uses.

⚠ WHY A BRIEF IS A PASEM AND NOT AN ATTRIBUTE. The footer is line-oriented; a
multi-line brief in an attribute would be shredded on the first newline. Each
task's verbatim brief is its own `[[pasem: brief-<id>]]` block, so the registry
stays one file, one lock, one atomic write.

PERMISSIONS — enforced in `authorise()` (below), never in prose:
  · Only an ASSIGNER may hand work to somebody else.
  · Any agent may CLAIM a task that is unassigned or already its own.
  · An agent may move only its OWN task through the lifecycle.
  · No agent may propose a task owned by a different agent, reassign another
    agent's task, or complete one. ⚠ Not even an ASSIGNER may complete somebody
    else's task — assigning work and reporting it finished are separate powers,
    and only OVERRIDE (the operator) holds the second.
  · `heartbeat` is a SYSTEM actor with two rights and no others: move an
    assigned task to `active` on delivery, and park one it refused as blocked.

Stdlib only, plus the trainer's `bmd.py` loaded by path through `liljack_board`.
"""
from __future__ import annotations

import hashlib
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import liljack_board as board                                      # noqa: E402
import liljack_mail as mail                                        # noqa: E402


# ── the roster ───────────────────────────────────────────────────────────────
# N agents, not 2. `liljack_mail.AGENTS` is the mailbox's fixed roster and this
# session may not edit that file, so a THIRD (fourth, …) agent is declared here
# through the environment: `LILJACK_AGENTS=chad,ada`. Everything below is written
# against `AGENTS`, never against a pair.
def _roster() -> tuple:
    extra = [a.strip().lower() for a in
             os.environ.get("LILJACK_AGENTS", "").replace(";", ",").split(",")]
    out = list(mail.AGENTS)
    for a in extra:
        if a and a.isidentifier() and a not in out:
            out.append(a)
    return tuple(out)


AGENTS = _roster()

# ⚠ WHO MAY ASSIGN. the operator decides who does what — that is the rule, and it is
# ONE mechanism, not a hardcoded list of agents. ANY agent can be the room lead
# (the operator 2026-09-11: "any agent can be lead"), and the lead hands work out; the
# set therefore resolves as `operator` + the named lead (LILJACK_LEAD), with the
# legacy delivery-loop default kept only when no lead is declared. `claude` is
# in that fallback because the operator put it in control of the delivery loop
# ("Uroboros style, with Claude in control", 2026-09-05). Narrow to the operator alone
# with `LILJACK_ASSIGNERS=operator` — every enforcement path still holds.
def _assigners() -> tuple:
    named = [a.strip().lower() for a in
             os.environ.get("LILJACK_ASSIGNERS", "").replace(";", ",").split(",")]
    named = tuple(a for a in named if a in AGENTS)
    if named:
        out = list(named)
    else:
        out = ["operator", "claude"]
    lead = os.environ.get("LILJACK_LEAD", "").strip().lower()
    if lead in AGENTS and lead not in out:
        out.append(lead)
    return tuple(out)


ASSIGNERS = _assigners()

# ⚠ ASSIGNING AND SUPERVISING ARE DIFFERENT POWERS, and conflating them was a
# real hole caught while exercising the MCP tools: with `claude` in ASSIGNERS,
# an assigner-bypass on the state check let Claude mark CODEX's task `done`.
# Marking somebody else's work finished is the precise falsehood this whole
# layer exists to prevent, so only THE OPERATOR — the human who decides — may move a
# task he does not own. An assigner may hand work over; he may not report it.
OVERRIDE = ("operator",)

SYSTEM = "heartbeat"                # the delivery daemon; not an agent
ACTORS = AGENTS + (SYSTEM,)

# ── the lifecycle ────────────────────────────────────────────────────────────
STATES = ("proposed", "assigned", "active", "held", "blocked", "done", "abandoned")
OPEN = ("proposed", "assigned", "active", "held", "blocked")
DELIVERABLE = "assigned"            # the ONLY state the heartbeat may hand out

# ⚠ PENDING is the ARMING predicate, and it is deliberately NOT the same as OPEN.
# `blocked` is OPEN but NOT PENDING: the work is present, yet nobody can act on it
# right now, so it must not arm the heartbeat — a loop armed on blocked work alone
# has nothing to deliver and either spams the owner or ticks empty forever. It
# must also not, on its own, keep the loop alive (the symmetric "all can rest").
# Blocked work still surfaces, twice over: on the attention board (via sync_board)
# and in the retire reason, which names the blocked ids so nothing rots silently.
#
# `held` is the DELIBERATE park, and it is also OPEN but NOT PENDING. `blocked`
# means "this cannot move" (a dependency, a decision); `held` means "the assigner
# chose to park it" — so it must not arm the heartbeat, must NOT be surfaced as a
# stuck task in the lead's digest, and ONLY the assigner may clear it. Before this
# existed the lead parked a task with `block` and it read as a dependency forever.
PENDING = ("proposed", "assigned", "active")

# Reached-from → reachable. A self-transition is legal (re-stating a state is a
# no-op that still stamps `updated`), which is what makes `start` idempotent.
TRANSITIONS = {
    "proposed":  {"proposed", "assigned", "held", "abandoned"},
    "assigned":  {"assigned", "proposed", "active", "held", "blocked", "done", "abandoned"},
    "active":    {"active", "assigned", "held", "blocked", "done", "abandoned"},
    # `held` is the assigner's park. LEAVING it is assigner-only (enforced in
    # set_state), so the owner cannot un-park its own work behind the assigner.
    "held":      {"held", "assigned", "proposed", "active", "blocked", "abandoned"},
    "blocked":   {"blocked", "assigned", "active", "held", "done", "abandoned"},
    # Terminal states re-open only through an ASSIGNER — an agent cannot
    # resurrect its own finished or abandoned work and quietly re-run it.
    "done":      {"done", "assigned", "proposed"},
    "abandoned": {"abandoned", "assigned", "proposed"},
}
_REOPEN_FROM = ("done", "abandoned")

PRED_TASK = "task"
PRED_LOG = "task-log"
PRED_OWNER = "task-owner"           # the predicate an explicit neg() denies

MAX_TITLE = 160
MAX_NOTE = 400


def _note(text) -> str:
    """Clip a note to MAX_NOTE and SAY SO when it clipped.

    ⚠ A SILENTLY TRUNCATED NOTE READS AS A COMPLETE ONE. deepseek closed
    owkterm-native-sessions with a note ending "One honest caveat:" — the caveat
    itself fell off the 400-character edge, and the note looked finished. The
    lead read a truncated sentence as the whole report and had to go digging
    through the design doc to find what had been cut.

    Same defect class as the UI labels fixed this morning (a clipped "toolbox"
    rendered "toolbo"): the fix is not a bigger limit, it is an HONEST one. The
    marker is what tells a reader to go look for the rest.
    """
    t = str(text or "")
    if len(t) <= MAX_NOTE:
        return t
    return t[:MAX_NOTE - 1] + "…"
MAX_FIELD = 600                     # acceptance / avoid / report
MAX_PROGRESS = 32
MAX_BRIEF = 12000
MAX_LOG = 400                       # newest transitions kept in the file
MAX_ID = 60
MAX_ROOM = 40                       # a workspace room id (r-<hex>), carried verbatim

ENVELOPE_VERSION = "v1"
REPORT_DEFAULT = ("task_update op=complete (or op=block with the reason) on THIS "
                  "task id. Never mark another agent's task.")


class TaskError(ValueError):
    """A bad request: unknown task, illegal transition, missing field."""


class Denied(PermissionError):
    """The actor is not allowed to do this. Raised by `authorise`, never logged
    away — a refused write must be visible to whoever asked for it."""


def now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def default_path() -> Path:
    return Path(os.environ.get("LILJACK_TASKS",
                               str(board.default_path().parent / "agent_tasks.bmd")))


# ══════════════════════════════════════════════════════════════════════════
# permissions — the whole rule set, in one function, so there is nowhere else
# for a second opinion to live
# ══════════════════════════════════════════════════════════════════════════

def is_assigner(actor: str) -> bool:
    return actor in ASSIGNERS


def session_scope(actor: str, task) -> None:
    """Raise `Denied` when a launched session acts, AS OWNER, on its agent's row
    that is bound to a different session.

    ⚠ AN AGENT NAME IS NOT A SESSION. Room r-bcbc0454 (2026-09-17): a deepseek
    worker read three ring-room rows bound to another deepseek session as its own
    work, and nothing stopped it moving them — authorise() compared provider names
    only. the operator: "fix the coordination bugs". Rows with no owner_session, the
    operator CLI (no LILJACK_WORKSPACE_SESSION) and the operator keep full reach."""
    if not task or (actor or "").strip().lower() in OVERRIDE:
        return
    mine = os.environ.get("LILJACK_WORKSPACE_SESSION", "").strip()
    bound = (task.get("owner_session") or "").strip()
    if mine and bound and bound != mine:
        where = f" in room {task.get('room')}" if task.get("room") else ""
        raise Denied(f"{task.get('id')} belongs to session {bound}{where}; you are session "
                     f"{mine} — it is not your work (ask the operator to reassign it)")


def authorise(actor: str, op: str, task=None, owner: str = "") -> str:
    """Return the normalised actor, or raise `Denied`.

    ops: propose · assign · state · progress · deliver · refuse · reopen
    `task` is the CURRENT row (None when creating); `owner` is the intended
    owner for `propose`/`assign`.
    """
    a = (actor or "").strip().lower()
    if a not in ACTORS:
        raise Denied(f"unknown actor {actor!r} — one of {ACTORS}")
    want = (owner or "").strip().lower()
    have = ((task or {}).get("owner") or "").strip().lower()

    # ── the system actor: two rights, and nothing resembling authorship ──
    if a == SYSTEM:
        if op not in ("deliver", "refuse"):
            raise Denied(f"{SYSTEM} may only deliver or refuse — not {op!r}")
        if not task:
            raise Denied(f"{SYSTEM} may not create a task")
        if task.get("state") != DELIVERABLE:
            raise Denied(f"{SYSTEM} may only act on an {DELIVERABLE} task "
                         f"({task.get('id')} is {task.get('state')})")
        if not have:
            raise Denied(f"{SYSTEM} may not act on an unowned task")
        return a

    if op == "propose":
        # Inventing work for somebody else is assignment wearing a hat.
        if want and want != a and not is_assigner(a):
            raise Denied(f"{a} may not create a task owned by {want} — "
                         f"only {ASSIGNERS} assign; propose it unowned instead")
        return a

    if op == "assign":
        if not want:
            raise Denied("assign needs an owner")
        if is_assigner(a):
            return a
        if want != a:
            raise Denied(f"{a} may not assign work to {want} — only {ASSIGNERS} assign")
        if have and have != a:
            raise Denied(f"{a} may not take {task.get('id')} from {have} — "
                         "it is already owned; ask an assigner to reassign it")
        return a                                   # a claim: unassigned, or mine

    if op == "reopen":
        if not is_assigner(a):
            raise Denied(f"{a} may not reopen {task.get('id')} from "
                         f"{task.get('state')} — only {ASSIGNERS} reopen")
        return a

    if op in ("state", "progress"):
        if not task:
            raise TaskError("no such task")
        if a in OVERRIDE:               # NOT is_assigner — see the note above
            return a
        if not have:
            raise Denied(f"{task.get('id')} has no owner — claim it before working it")
        if have != a:
            raise Denied(f"{a} may not change {task.get('id')}: it belongs to {have}")
        session_scope(a, task)
        return a

    raise Denied(f"unknown operation {op!r}")


# ══════════════════════════════════════════════════════════════════════════
# storage — one BMD file, one lock, one atomic write (reused from the board)
# ══════════════════════════════════════════════════════════════════════════

_UNSAFE_LINE = "[["
# ⚠ INDENTING IS NOT ENOUGH. `bmd.parse` STRIPS each line before comparing it to
# `[[/pasem]]`, so a brief containing that marker — indented or not — closed its
# own container and the rest of the brief was silently lost (measured: a
# three-line brief came back as one line). The escape has to change the BYTES:
# a zero-width space between the brackets, invisible in the rendered brief and
# removed again on read, so the round trip is exact.
_ESC = "\u200b"
_ESCAPED = "[" + _ESC + "["


def _clean_brief(text: str) -> str:
    """A brief is stored VERBATIM inside a pasem. The one transform is the escape
    the container demands, and it is reversed by `_unescape_brief` on read."""
    out = []
    for line in str(text).replace("\r\n", "\n").split("\n"):
        out.append(line.replace(_UNSAFE_LINE, _ESCAPED)
                   if line.lstrip().startswith(_UNSAFE_LINE) else line)
    return "\n".join(out).strip()[:MAX_BRIEF]


def _unescape_brief(text: str) -> str:
    return str(text).replace(_ESCAPED, _UNSAFE_LINE)


# Every attribute this version writes explicitly. Anything else a row carries is
# preserved verbatim through `_extra` rather than dropped.
_KNOWN_ATTRS = frozenset((
    "title", "owner", "owner_session", "room", "by", "progress", "note",
    "acceptance", "avoid", "report", "source", "mail", "session", "blocked_on",
    # ⚠ RANK, DON'T JUST AGE. The heartbeat offered the OLDEST eligible task, so a
    # parked low-priority item could be handed out ahead of the work the room was
    # actually on. `priority` is a lead-set integer; higher = more important;
    # absent/0 = the old behaviour (state order, then age), so nothing changes
    # until an assigner ranks something. It orders the nudge set and the registry
    # listing, never authority.
    "priority",
    # ⚠ HOW OFTEN THIS TASK IS WORTH ASKING ABOUT, in seconds. A WATCH task
    # cannot be finished on demand — trio-relaunch monitors a 17-hour training
    # run — so the heartbeat's generic 20-minute freshness window is simply the
    # wrong rhythm for it: its owner reports "still healthy", ages out, and is
    # asked again. Measured 2026-09-10: three nudges in one hour on a run that
    # needed nothing. Absent means the default, so every existing task is
    # unchanged; a task that knows its own cadence declares it.
    "checkin",
    "created", "updated"))


def _row(a) -> dict:
    return {"id": str(a.args[0]),
            "state": str(a.args[1]) if len(a.args) > 1 else "proposed",
            "title": a.attrs.get("title", ""),
            "owner": a.attrs.get("owner", ""),
            "owner_session": a.attrs.get("owner_session", ""),
            "room": a.attrs.get("room", ""),
            "by": a.attrs.get("by", ""),
            "progress": a.attrs.get("progress", ""),
            "note": a.attrs.get("note", ""),
            "acceptance": a.attrs.get("acceptance", ""),
            "avoid": a.attrs.get("avoid", ""),
            "report": a.attrs.get("report", ""),
            "source": a.attrs.get("source", ""),
            "mail": a.attrs.get("mail", ""),
            "session": a.attrs.get("session", ""),
            # ⚠ CARRY FORWARD WHAT THIS VERSION DOES NOT UNDERSTAND. A writer
            # that rebuilds a row from a fixed field list DELETES every field it
            # has never heard of — for every task, not just the one it touched.
            # Measured 2026-09-10: a pre-schema MCP server updated one task and
            # `room` and `blocked_on` vanished from all fourteen. Keeping the
            # unknown attributes means the NEXT field added cannot be erased by
            # a process that predates it.
            "_extra": {k: v for k, v in a.attrs.items() if k not in _KNOWN_ATTRS},
            # ⚠ The task this one WAITS FOR. Read and written like any other
            # field, because a dependency kept only in prose is one no code can
            # act on — which is why finished work left blocked work blocked.
            "blocked_on": a.attrs.get("blocked_on", ""),
            # Seconds between check-ins; "" means the heartbeat's default.
            "checkin": a.attrs.get("checkin", ""),
            # Lead-set nudge rank; "" / 0 means the default (state, then age).
            "priority": a.attrs.get("priority", ""),
            "created": a.attrs.get("created", ""),
            "updated": a.attrs.get("updated", "")}


def _log_row(a) -> dict:
    return {"id": str(a.args[0]),
            "state": str(a.args[1]) if len(a.args) > 1 else "",
            "by": a.attrs.get("by", ""), "at": a.attrs.get("at", ""),
            "op": a.attrs.get("op", ""), "note": a.attrs.get("note", "")}


def read(path=None) -> dict:
    """Parse the registry. Lenient fallback, like the board: one hand-edited bad
    line must not blind both harnesses, but it IS counted."""
    p = Path(path) if path else default_path()
    B = board.bmd()
    out = {"path": str(p), "exists": p.exists(), "tasks": [], "log": [], "negs": [],
           "briefs": {}, "errors": 0, "updated": "", "header": {}}
    if not p.exists():
        return out
    text = p.read_text(encoding="utf-8", errors="replace")
    try:
        doc = B.parse(text, strict=True)
    except B.BmdError:
        doc = B.parse(text, strict=False)
        out["errors"] = 1
    out["header"] = dict(doc.header)
    out["updated"] = doc.header.get("updated", "")
    for ps in doc.pasems:
        if ps.name.startswith("brief-"):
            out["briefs"][ps.name[len("brief-"):]] = _unescape_brief(ps.text.strip())
    for a in doc.assertions:
        if a.form == "fact" and a.pred == PRED_TASK and len(a.args) >= 2:
            out["tasks"].append(_row(a))
        elif a.form == "fact" and a.pred == PRED_LOG and len(a.args) >= 1:
            out["log"].append(_log_row(a))
        elif a.form == "neg":
            out["negs"].append({"pred": a.pred, "args": [str(x) for x in a.args],
                                "reason": a.attrs.get("reason", ""),
                                "updated": a.attrs.get("updated", "")})
    for t in out["tasks"]:
        t["brief"] = out["briefs"].get(t["id"], "")
    return out


_STATE_MARK = {"proposed": "·", "assigned": "○", "active": "▶", "held": "⏸",
               "blocked": "⛔", "done": "✓", "abandoned": "✗"}
_STATE_ORDER = {"active": 0, "blocked": 1, "assigned": 2, "proposed": 3,
                "held": 4, "done": 5, "abandoned": 6}


def _prio(t) -> int:
    """A task's nudge rank. Malformed or absent means 0 — never a crash."""
    try:
        return int(t.get("priority") or 0)
    except (TypeError, ValueError):
        return 0


def _prose(tasks: list, negs: list) -> str:
    """The readable summary — DERIVED on every write. A hand-written one is a
    second source of truth, and the whole point of this file is that there is
    exactly one."""
    lines = ["Every task, its owner and its state. GENERATED from the claims in",
             "the footer on every write — change a task through liljack_tasks",
             "(or the task_* MCP tools), never by editing this text.",
             "",
             "lifecycle: proposed → assigned → active → done  "
             "(+ blocked, held, abandoned; only `assigned` is deliverable; "
             "`held` is an assigner park the heartbeat skips)",
             f"assigners: {', '.join(ASSIGNERS)} · agents: {', '.join(AGENTS)}",
             ""]
    for ag in list(AGENTS) + ["(unassigned)"]:
        mine = [t for t in tasks if (t["owner"] or "(unassigned)") == ag]
        if not mine:
            continue
        mine.sort(key=lambda t: (_STATE_ORDER.get(t["state"], 9), t["created"]))
        lines.append(f"{ag}:")
        for t in mine:
            bit = f"  {_STATE_MARK.get(t['state'], '?')} {t['id']} — {t['state']}"
            if t["room"]:
                bit += f" · room {t['room']}"
            if t["progress"]:
                bit += f" {t['progress']}"
            if t["title"]:
                bit += f" · {t['title']}"
            lines.append(bit)
            if t["note"]:
                lines.append(f"      note: {t['note']}")
    if negs:
        lines += ["",
                  "explicitly NOT the case (the engine has no negation-as-failure,",
                  "so an absence is written down rather than left to be inferred):"]
        for n in negs:
            bit = "  not " + "/".join(n["args"])
            if n["reason"]:
                bit += f" — {n['reason']}"
            lines.append(bit)
    return "\n".join(lines)


def _render(tasks: list, log: list, negs: list, briefs: dict, header: dict) -> str:
    B = board.bmd()
    doc = B.BmdDoc()
    doc.header = dict(header)
    doc.header.update({"title": "lilJack agent task registry", "kind": "agent-tasks",
                       "domain": "liljack", "version": "1", "updated": now()})
    doc.pasems = [B.Pasem(name="tasks", text=_prose(tasks, negs))]
    for t in tasks:
        text = briefs.get(t["id"], "")
        if text:
            doc.pasems.append(B.Pasem(name=f"brief-{t['id']}", text=text))
    out = []
    for t in tasks:
        out.append(B.fact(PRED_TASK, t["id"], t["state"], **board.qattr_all(
            title=t["title"], owner=t["owner"], owner_session=t["owner_session"],
            room=t["room"], by=t["by"], progress=t["progress"], note=t["note"],
            acceptance=t["acceptance"], avoid=t["avoid"], report=t["report"],
            source=t["source"], mail=t["mail"], session=t["session"],
            blocked_on=t.get("blocked_on", ""),
            # ⚠ RENDERED, OR DROPPED. `_row` reads these as KNOWN attrs, so they
            # are excluded from `_extra` — and an attribute missing from this
            # list is silently deleted on every save. `checkin` was lost this way
            # (the cadence never persisted); `priority` would have been too.
            checkin=t.get("checkin", ""), priority=t.get("priority", ""),
            created=t["created"], updated=t["updated"],
            **{k: v for k, v in (t.get("_extra") or {}).items()
               if k not in _KNOWN_ATTRS})))
    for r in log[-MAX_LOG:]:
        out.append(B.fact(PRED_LOG, r["id"], r["state"] or "note", **board.qattr_all(
            op=r["op"], by=r["by"], at=r["at"], note=r["note"])))
    for n in negs:
        out.append(B.Assertion("neg", n["pred"], list(n["args"]),
                               board.qattr_all(reason=n["reason"], updated=n["updated"])))
    doc.assertions = out
    return doc.render()


def _save(store: dict, path: Path) -> None:
    board.write_atomic(path, _render(store["tasks"], store["log"], store["negs"],
                                     store["briefs"], store["header"]))
    _mirror_after_save(store, path)


def _mirror_after_save(store: dict, path: Path) -> None:
    """Mirror the registry onto the attention board after EVERY write.

    ⚠ THE MIRROR BELONGS TO THE WRITE, NOT TO ONE CALLER. It used to live only
    in `liljack_mcp._mirror()`, so a write through MCP updated the board and a
    write through the library or the CLI did not — and the two stores then
    disagreed forever, silently. Measured 2026-09-10: `room-phase-ui`,
    `room-phase-machine` and `room-question-routing` were `done` in the
    registry and still `active` on the board, because those completions were
    written through `python3 -c` (the interim rule while the MCP servers held
    stale modules). The heartbeat unions the board into its nudge set, so it
    nudged their owners about finished work. Putting the mirror in `_save`
    means the board follows the registry no matter who writes.

    ⚠ ONLY THE REAL REGISTRY MIRRORS. Every test writes a temp registry; if a
    temp write mirrored, running the suite would rewrite the LIVE attention
    board with fixture rows. The path check is what makes the hook safe.

    ⚠ The board is a VIEW. A board failure must never lose a registry write —
    the registry is already durably on disk by the time we get here.
    """
    try:
        registry = Path(path)
        if registry != default_path():
            return
        # ⚠ CHECKING THE REGISTRY PATH ALONE IS NOT ENOUGH, AND THAT HOLE WAS
        # LIVE FOR AN HOUR. A test that redirects LILJACK_TASKS to a temp file
        # and leaves LILJACK_BOARD alone makes default_path() return the TEMP
        # registry — so the check above passes, and the mirror writes fixture
        # rows into the REAL attention board. Measured 2026-09-10 06:13Z: six
        # rows (worker-private-task, reviewer-private-task, unscoped-work,
        # verify-operator-path, never-copy-todo, lead-task) from
        # test_liljack_room_protocol.py landed on the live board and entered the
        # heartbeat's nudge set as real work for codex.
        #
        # The invariant is that a registry only mirrors onto the board BESIDE
        # IT. True for the real pair (both in the trainer/data/braid) and for a
        # test that redirects both into one temp dir; false for exactly the
        # mismatch above. It cannot be satisfied by accident.
        if board.default_path().parent != registry.parent:
            return
        _mirror_rows(store["tasks"])
    except Exception:
        pass


def _find(store: dict, task_id: str) -> dict:
    # ⚠ EXACT ID FIRST. propose() suffixes a taken id AFTER normalising it, so a
    # long title's duplicate is "<MAX_ID chars>-2"; normalising that again cut the
    # suffix and resolved to the ORIGINAL row — the duplicate of a second room
    # start could not be addressed at all (found fixing coord-d, 2026-09-17).
    raw = str(task_id or "").strip()
    row = next((t for t in store["tasks"] if t["id"] == raw), None)
    if row is not None:
        return row
    tid = _norm_id(task_id)
    row = next((t for t in store["tasks"] if t["id"] == tid), None)
    if row is None:
        raise TaskError(f"no such task: {task_id!r}")
    return row


def _norm_id(task_id: str) -> str:
    return board.bmd().slug(str(task_id or ""), maxlen=MAX_ID)


def _log(store: dict, task_id: str, state: str, op: str, by: str, note: str = "") -> None:
    store["log"].append({"id": task_id, "state": state, "op": op, "by": by,
                         "at": now(), "note": _note(note)})


# ══════════════════════════════════════════════════════════════════════════
# writes — every one of them goes through authorise() first
# ══════════════════════════════════════════════════════════════════════════

def _bind_to_caller_session(row: dict, actor: str) -> None:
    """A launched session that takes a row as its OWN owner binds it: owner_session
    from LILJACK_WORKSPACE_SESSION, room from that session's room — each only when
    blank. ⚠ MULTI-ROOM (the operator 2026-09-20, r-bcbc0454): a row with no room is shown
    by every room's context fallback that matches the agent name and is nudged by
    NONE (worker_wake.room_allows never lands a room-less row in a room). Filing it
    at claim/start time is what makes the registry answer "whose room?"."""
    sess = os.environ.get("LILJACK_WORKSPACE_SESSION", "").strip()
    if not sess or actor != (row.get("owner") or ""):
        return
    if not (row.get("owner_session") or "").strip():
        row["owner_session"] = sess[:80]
    if not (row.get("room") or "").strip():
        try:
            room = mail.session_room(sess)
        except Exception:
            room = ""
        if room:
            row["room"] = str(room).strip()[:120]


def propose(title: str, by: str, owner: str = "", brief: str = "", acceptance: str = "",
            avoid: str = "", note: str = "", report: str = None, task_id: str = "",
            state: str = "", source: str = "", mail_id: str = "", room: str = "",
            owner_session: str = "", priority: int = 0, path=None) -> dict:
    """Create a task. Unowned unless the caller may assign (or owns it itself).

    A task created with an owner is created `assigned`; without one, `proposed`.
    `room` is the workspace room id (`r-<hex>`) the task belongs to; `owner_session`
    is the exact session that owns it — Codex owns room creation and seeds a
    room's first todo with these, because `owner=` alone (a provider) cannot tell
    two sessions of the same harness apart. Both are carried verbatim through
    every later transition and never changed by assign/claim.
    """
    title = str(title or "").strip()
    if not title:
        raise TaskError("title is required")
    owner = (owner or "").strip().lower()
    if owner and owner not in AGENTS:
        raise TaskError(f"owner must be one of {AGENTS}")
    room = str(room or "").strip()[:MAX_ROOM]
    owner_session = str(owner_session or "").strip()[:80]
    try:
        priority = int(priority or 0)
    except (TypeError, ValueError):
        raise TaskError(f"priority must be a whole number, got {priority!r}")
    actor = authorise(by, "propose", None, owner)
    st = (state or ("assigned" if owner else "proposed")).strip().lower()
    if st not in STATES:
        raise TaskError(f"state must be one of {STATES}")
    if st != "proposed" and not owner:
        raise TaskError(f"a task in state {st!r} needs an owner")

    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        tid = _norm_id(task_id or title)
        taken = {t["id"] for t in store["tasks"]}
        if tid in taken:
            n = 2
            while f"{tid}-{n}" in taken:
                n += 1
            tid = f"{tid}-{n}"
        row = {"id": tid, "state": st, "title": title[:MAX_TITLE], "owner": owner,
               "owner_session": owner_session, "room": room, "by": actor,
               "progress": "", "note": _note(note),
               "acceptance": str(acceptance or "")[:MAX_FIELD],
               "avoid": str(avoid or "")[:MAX_FIELD],
               "report": (REPORT_DEFAULT if report is None else str(report))[:MAX_FIELD],
               "source": str(source or "")[:60], "mail": str(mail_id or "")[:60],
               "session": "", "priority": str(int(priority or 0)),
               "created": now(), "updated": now()}
        store["tasks"].append(row)
        if brief:
            store["briefs"][tid] = _clean_brief(brief)
        _log(store, tid, st, "propose", actor, note)
        _save(store, p)
        return dict(row, brief=store["briefs"].get(tid, ""))


def _audit_assignment(row: dict, actor: str, note: str = "", path=None) -> None:
    """the operator 2026-09-05: an assigner other than the operator may hand out work, but
    every such assignment is RECORDED where he can see it without reading the
    registry — a mail to him and a dated note on the board. Best effort: an
    audit trail that could refuse the assignment would be worse than one that
    occasionally misses a line, so failures here never block the decision.
    """
    if actor == "operator":
        return                                   # his own call needs no notice
    # ⚠ Only audit a real assignment. The first cut wrote unconditionally and
    # the TEST SUITE — which points the registry at a temp file — pushed its
    # fixtures ("chad", "codex-work") into the LIVE board and the operator's live
    # mailbox. A side effect that escapes its own test root is a side effect
    # in production. Anything but the default registry is a test or a sandbox.
    if Path(path or default_path()).resolve() != default_path().resolve():
        return
    what = f"{actor} assigned {row['id']} to {row['owner']}"
    try:
        mail.send(f"{what}.\n\ntitle: {row.get('title','')}\nstate: {row['state']}"
                  f"\nnote: {note or '(none)'}\n\nReverse it with: "
                  f"liljack_tasks.py --by operator unassign {row['id']}  (or assign it elsewhere).",
                  "operator", frm=actor, subject=f"ASSIGNMENT: {row['id']} -> {row['owner']}",
                  project="demo", thread="task-assignments")
    except Exception:
        pass
    try:
        board.set_note(f"assignment/{row['id']}", row["owner"], reason=f"{what}; {note}"[:300])
    except Exception:
        pass


def assign(task_id: str, owner: str, by: str, note: str = "", path=None) -> dict:
    """Hand a task to an agent. An ASSIGNER may hand it to anyone; anybody else
    may only CLAIM — take a task that is unassigned or already its own."""
    owner = (owner or "").strip().lower()
    # ⚠ Identity before arguments: `claim(id, by="nobody")` passes the actor as
    # the owner too, and validating the owner first turned an unknown ACTOR into
    # a complaint about the owner list — the right refusal for the wrong reason.
    if (by or "").strip().lower() not in ACTORS:
        raise Denied(f"unknown actor {by!r} — one of {ACTORS}")
    if owner not in AGENTS:
        raise TaskError(f"owner must be one of {AGENTS}")
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "assign", row, owner)
        target = "assigned"
        if row["state"] in _REOPEN_FROM:
            authorise(actor, "reopen", row)          # a terminal task re-opens
        if target not in TRANSITIONS.get(row["state"], set()):
            raise TaskError(f"{row['id']}: {row['state']} → {target} is not a legal move")
        row.update(owner=owner, state=target, updated=now())
        _bind_to_caller_session(row, actor)      # a claim from a room files the row there
        if note:
            row["note"] = _note(note)
        # A positive owner claim retracts its own explicit denial.
        store["negs"] = [n for n in store["negs"]
                         if not (n["pred"] == PRED_OWNER and n["args"][:2] == [row["id"], owner])]
        _log(store, row["id"], target, "assign", actor, note or f"owner={owner}")
        _save(store, p)
        out = dict(row)
    _audit_assignment(out, actor, note, p)       # outside the lock — it writes the board
    return out


def claim(task_id: str, by: str, note: str = "", path=None) -> dict:
    """Take an unassigned task (or re-take your own). Sugar over `assign`, and
    it is sugar on purpose — the permission check is the same one."""
    return assign(task_id, by, by, note=note, path=path)


def set_state(task_id: str, state: str, by: str, note: str = "", progress: str = None,
              path=None) -> dict:
    state = (state or "").strip().lower()
    if state not in STATES:
        raise TaskError(f"state must be one of {STATES}")
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "state", row)
        # ⚠ ONLY THE ASSIGNER CLEARS A PARK. `held` is a deliberate assigner/lead
        # decision, so an owner may hold its own task but must not un-park it
        # behind the assigner's back.
        if row["state"] == "held" and state != "held" and not is_assigner(actor):
            raise Denied(f"{actor} may not clear held task {row['id']} — only {ASSIGNERS} un-park it")
        if row["state"] in _REOPEN_FROM and state not in _REOPEN_FROM:
            authorise(actor, "reopen", row)
        if state not in TRANSITIONS.get(row["state"], set()):
            raise TaskError(f"{row['id']}: {row['state']} → {state} is not a legal move")
        if state == "blocked" and not note:
            raise TaskError("blocking a task requires a note saying what blocks it")
        # ⚠ LEAVING `blocked` CLEARS THE DEPENDENCY. Otherwise a task that is
        # no longer blocked keeps claiming to wait on something — unassign()
        # moves a blocked task to `proposed` and used to leave blocked_on set,
        # so blocked_by() would report a waiter that was not waiting.
        if row["state"] == "blocked" and state != "blocked":
            row["blocked_on"] = ""
        # ⚠ STARTING A TASK NAMES THE SESSION THAT STARTED IT. A row with a blank
        # owner_session was routed by agent slot into any room; the session that
        # actually took the work is the one the heartbeat must keep asking.
        if state == "active":
            _bind_to_caller_session(row, actor)   # carried by the transition log row below, not a separate op
        row["state"] = state
        row["updated"] = now()
        if note:
            row["note"] = _note(note)
        if progress is not None:
            row["progress"] = str(progress).strip()[:MAX_PROGRESS]
        _log(store, row["id"], state, "state", actor, note)
        _save(store, p)
        return dict(row)


def start(task_id, by, note="", path=None):
    return set_state(task_id, "active", by, note, path=path)


def block(task_id, by, note, path=None, blocked_on: str = ""):
    """Block a task, optionally naming the task it WAITS FOR.

    ⚠ A DEPENDENCY MUST BE A FIELD, NOT PROSE. Until now `note` was the only
    record of what a blocked task was waiting for, so nothing could read it:
    DeepSeek finished the work two of Codex's tasks were blocked on and they
    stayed blocked, because no code anywhere could tell they were related.
    the operator, 2026-09-10: "why ds finished and no todos are unbloked no heartbeat
    spanking agents? lets fix that scenario." `blocked_on` is that scenario's
    missing half; `release()` is the other.
    """
    value = set_state(task_id, "blocked", by, note, path=path)
    if blocked_on:
        pth = Path(path) if path else default_path()
        with board.lock(pth):
            store = read(pth)
            row = _find(store, task_id)
            row["blocked_on"] = str(blocked_on).strip()[:120]
            # Logged as its own op so the dependency is RECOVERABLE the same way
            # `room` is — the log survived the 2026-09-10 clobber when the rows
            # did not, and that is only useful for fields the log actually names.
            _log(store, row["id"], row["state"], "blocked_on", by, row["blocked_on"])
            _save(store, pth)
            value = dict(row)
    return value


def release(task_id: str, by: str, path=None) -> list:
    """Return every task blocked on `task_id` to `assigned`. Returns their ids.

    Called when a task reaches a terminal state, so finishing work actually
    unblocks the work that waited for it. An unowned blocked task goes back to
    `proposed`, because `assigned` with no owner is not a state anyone can act
    on. Blocking notes are PRESERVED and appended to, never overwritten — the
    reason it was blocked is history worth keeping.
    """
    pth = Path(path) if path else default_path()
    freed = []
    with board.lock(pth):
        store = read(pth)
        for row in store["tasks"]:
            if row.get("state") != "blocked":
                continue
            if (row.get("blocked_on") or "").strip() != task_id:
                continue
            row["state"] = "assigned" if row.get("owner") else "proposed"
            row["blocked_on"] = ""
            row["updated"] = now()
            row["note"] = _note(f"released: {task_id} is done. " + str(row.get("note") or ""))
            _log(store, row["id"], row["state"], "state", by, f"released by {task_id}")
            freed.append(row["id"])
        if freed:
            _save(store, pth)
    return freed


def set_checkin(task_id: str, seconds, by: str, path=None) -> dict:
    """How often this task is worth asking about. 0/"" restores the default.

    ⚠ A WATCH TASK CANNOT BE FINISHED ON DEMAND. The heartbeat's freshness
    window is one number for every task, and for a monitor of a 17-hour
    training run that number is wrong: the owner reports "still healthy", ages
    out of a 20-minute window, and is asked again. Measured 2026-09-10: three
    nudges in one hour about a run that needed nothing, each one costing a turn
    to answer with the same answer.

    This is NOT a mute. The task still nudges, still strikes, still escalates —
    it simply declares the rhythm at which asking is useful. Absent means the
    default, so every existing task behaves exactly as before.
    """
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "state", row)      # the owner or an assigner
        try:
            v = int(seconds or 0)
        except (TypeError, ValueError):
            raise TaskError(f"checkin must be a whole number of seconds, got {seconds!r}")
        if v < 0:
            raise TaskError("checkin cannot be negative")
        row["checkin"] = str(v) if v else ""
        row["updated"] = now()
        _log(store, row["id"], row["state"], "checkin", actor, row["checkin"])
        _save(store, p)
        return dict(row)


def set_priority(task_id: str, value, by: str, path=None) -> dict:
    """Set the nudge rank. Higher = more important; 0 restores the default order.

    ⚠ RANK, DON'T JUST AGE. The heartbeat offered the OLDEST eligible task, so a
    parked item could be handed out ahead of the work the room was actually on.
    The rank is an ASSIGNER's call (same authority as assigning the work), so an
    owner cannot promote its own task above the queue.
    """
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "state", row)      # owner or assigner
        if not is_assigner(actor):
            raise Denied(f"{actor} may not rank {row['id']} — only {ASSIGNERS} set priority")
        try:
            v = int(value or 0)
        except (TypeError, ValueError):
            raise TaskError(f"priority must be a whole number, got {value!r}")
        row["priority"] = str(v) if v else ""
        row["updated"] = now()
        _log(store, row["id"], row["state"], "priority", actor, row["priority"])
        _save(store, p)
        return dict(row)


def hold(task_id: str, by: str, note: str = "", path=None) -> dict:
    """Park a task deliberately. The heartbeat skips it; only an assigner clears it."""
    return set_state(task_id, "held", by, note or "held by assigner", path=path)


def unhold(task_id: str, by: str, state: str = "assigned", note: str = "", path=None) -> dict:
    """Clear a park. Assigner-only (enforced by set_state)."""
    return set_state(task_id, state, by, note, path=path)


def set_room(task_id: str, room: str, by: str, path=None) -> dict:
    """Scope an existing task to a room, or clear it with room="".

    ⚠ ROOM WAS WRITE-ONCE AND THAT MADE THE ROOM TODO PANEL EMPTY. `propose`
    accepted room=, nothing else could set it, so every task filed before its
    room existed stayed invisible to room_tasks() and the ACTIONABLE tab showed
    "no todos scoped to this room" while fifteen were open. Same shape as the
    dependency defect: a field that only one code path can write is a field the
    rest of the system cannot use.
    """
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "state", row)      # the owner or an assigner
        row["room"] = str(room or "").strip()[:120]
        row["updated"] = now()
        # ⚠ LOG THE VALUE, NOT A WORD FOR IT. Logging "cleared" made the note
        # indistinguishable from a room literally named that, and the repair
        # below restored `room="cleared"` on a field somebody had deliberately
        # emptied. An empty note IS the record of a clear.
        _log(store, row["id"], row["state"], "room", actor, row["room"])
        _save(store, p)
        return dict(row)


def reconcile_scopes(by: str = SYSTEM, path=None) -> list:
    """Restore `room` and `blocked_on` from the LOG for rows that lost them.

    ⚠ A GUARD CANNOT CONSTRAIN A WRITER WE DO NOT CONTROL. On 2026-09-10 a
    process running a module older than these fields rebuilt every row from a
    fixed list and deleted both, for all fourteen tasks, while touching one —
    and it will keep being able to until its owner refreshes it. What survived
    was the LOG, because appending to it does not require understanding the
    fields. So the damage repairs itself from the record instead of waiting for
    a human to notice: the newest `room` / `blocked_on` entry wins, and a row
    that legitimately has neither is left alone.

    ⚠ A CLEARED FIELD IS NOT A LOST ONE. `set_room(id, "")` and leaving the
    blocked state both log the clearing, so the newest entry being empty means
    deliberately cleared and nothing is restored.
    """
    pth = Path(path) if path else default_path()
    fixed = []
    with board.lock(pth):
        store = read(pth)
        last = {}
        for entry in store["log"]:                       # oldest first; newest wins
            if entry.get("op") in ("room", "blocked_on"):
                last[(entry["id"], entry["op"])] = str(entry.get("note") or "")
        for row in store["tasks"]:
            for field in ("room", "blocked_on"):
                known = last.get((row["id"], field))
                if not known or (row.get(field) or "").strip():
                    continue                             # nothing recorded, or still present
                if field == "blocked_on" and row.get("state") != "blocked":
                    continue                             # only a blocked row waits on anything
                row[field] = known
                fixed.append(f'{row["id"]}.{field}')
        if fixed:
            _log(store, "*", "", "repair", by, f"restored {len(fixed)} field(s) from the log")
            _save(store, pth)
    return fixed


def reconcile_blocked(by: str = SYSTEM, path=None) -> list:
    """Release anything blocked on a dependency that is already finished.

    ⚠ WITHOUT THIS, A COMPLETION THAT NOBODY WATCHED LEAVES WORK BLOCKED FOREVER.
    complete() releases its waiters, but only if something is running to do it —
    and on 2026-09-10 the dependency landed while the heartbeat loop was still
    executing a module it had loaded fourteen minutes earlier, so the release
    never happened and two tasks stayed blocked with their dependency done.
    A blocked task whose dependency is gone from the registry entirely is
    released too: waiting on something that does not exist is not waiting.
    """
    pth = Path(path) if path else default_path()
    freed = []
    with board.lock(pth):
        store = read(pth)
        index = {r["id"]: r for r in store["tasks"]}
        for row in store["tasks"]:
            dep = (row.get("blocked_on") or "").strip()
            if row.get("state") != "blocked" or not dep:
                continue
            target = index.get(dep)
            if target is not None and target.get("state") not in _REOPEN_FROM:
                continue                       # the dependency is still open
            why = "is done" if target is not None else "no longer exists"
            row["state"] = "assigned" if row.get("owner") else "proposed"
            row["blocked_on"] = ""
            row["updated"] = now()
            row["note"] = _note(f"released: {dep} {why}. " + str(row.get("note") or ""))
            _log(store, row["id"], row["state"], "state", by, f"reconciled: {dep} {why}")
            freed.append(row["id"])
        if freed:
            _save(store, pth)
    return freed


def blocked_by(task_id: str, path=None) -> list:
    """Ids of the open tasks waiting on `task_id`."""
    return [t["id"] for t in tasks(open_only=True, path=path)
            if (t.get("blocked_on") or "").strip() == task_id]


def complete(task_id, by, note="", path=None):
    """Finish a task AND release whatever was waiting on it."""
    value = set_state(task_id, "done", by, note, path=path)
    freed = release(task_id, by, path=path)
    if freed:
        value = dict(value, released=freed)
    return value


def abandon(task_id, by, note, path=None):
    return set_state(task_id, "abandoned", by, note, path=path)


def set_progress(task_id: str, progress: str, by: str, note: str = None, path=None) -> dict:
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "progress", row)
        row["progress"] = str(progress).strip()[:MAX_PROGRESS]
        if note is not None:
            row["note"] = _note(note)
        row["updated"] = now()
        _log(store, row["id"], row["state"], "progress", actor, f"progress={row['progress']}")
        _save(store, p)
        return dict(row)


def unassign(task_id: str, by: str, reason: str = "", path=None) -> dict:
    """Take a task off an agent and SAY SO.

    ⚠ The `neg()` is the point. Dropping the `owner=` attribute would leave the
    absence to be inferred, and the toolbox_root engine cannot read an absent claim as
    false — "codex is not on this any more" has to be a claim."""
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "assign", row, row["owner"] or by)
        if not is_assigner(actor) and row["owner"] != actor:
            raise Denied(f"{actor} may not unassign {row['id']} from {row['owner']}")
        if row["owner"] == actor:
            session_scope(actor, row)
        was = row["owner"]
        # ⚠ Standing an owner down takes the task out of `blocked`, so its
        # dependency has to go with it — a `proposed` task still claiming
        # blocked_on would be reported by blocked_by() as a waiter that is not
        # waiting, and released later by a completion it no longer depends on.
        row.update(owner="", state="proposed", blocked_on="", updated=now())
        if was:
            store["negs"] = [n for n in store["negs"]
                             if not (n["pred"] == PRED_OWNER and n["args"][:2] == [row["id"], was])]
            store["negs"].append({"pred": PRED_OWNER, "args": [row["id"], was],
                                  "reason": _note(reason), "updated": now()})
        _log(store, row["id"], "proposed", "unassign", actor, reason or f"was {was}")
        _save(store, p)
        return dict(row)


def link_mail(task_id: str, mail_id: str, by: str, path=None) -> dict:
    """Record that this task IS the work a given `TASK:` message asked for.

    ⚠ This is what stops the importer creating a twin. `import_mail` dedups on
    the message id stamped into a task, so a task entered by hand for work that
    arrived as mail has to carry that id or the mail will be imported again as a
    second, competing task. Assigner-only: linking a message to a task is an
    assertion about what somebody was asked to do.
    """
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "assign", row, row["owner"] or by)
        if not is_assigner(actor):
            raise Denied(f"{actor} may not link a message to a task — only {ASSIGNERS}")
        row["mail"] = str(mail_id or "")[:60]
        row["source"] = row["source"] or "mail"
        row["updated"] = now()
        _log(store, row["id"], row["state"], "link-mail", actor, f"mail={row['mail']}")
        _save(store, p)
        return dict(row)


# ── the system actor's two rights ────────────────────────────────────────────

def deliver(task_id: str, session: str = "", by: str = SYSTEM, path=None) -> dict:
    """Mark a task delivered: assigned → active, stamped with the session it went
    to. `done` is never set here — finishing is the agent's own report."""
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "deliver", row)
        row.update(state="active", session=str(session or "")[:80], updated=now())
        _log(store, row["id"], "active", "deliver", actor,
             f"delivered to {session}" if session else "delivered")
        _save(store, p)
        return dict(row)


def refuse(task_id: str, reason: str, by: str = SYSTEM, path=None) -> dict:
    """Park a task the watcher would not deliver. It stays owned and visible —
    a refused task must not silently vanish the way an acked message did."""
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        row = _find(store, task_id)
        actor = authorise(by, "refuse", row)
        row.update(state="blocked", note=_note(reason), updated=now())
        _log(store, row["id"], "blocked", "refuse", actor, reason)
        _save(store, p)
        return dict(row)


def rerender(path=None) -> dict:
    """Re-emit the file from its own claims, changing no task and logging nothing.

    The generated prose is derived from the claims on every write, so an
    existing registry keeps whatever wording it was last written with until
    something touches it. This is the migration hook: it makes the prose agree
    with the current renderer without inventing a transition to hang it on.
    """
    p = Path(path) if path else default_path()
    with board.lock(p):
        store = read(p)
        _save(store, p)
        return {"tasks": len(store["tasks"]), "path": str(p)}


# ══════════════════════════════════════════════════════════════════════════
# reads
# ══════════════════════════════════════════════════════════════════════════

def get(task_id: str, path=None) -> dict:
    return _find(read(path), task_id)


def tasks(owner: str = "", state: str = "", open_only: bool = False, room: str = "",
          path=None) -> list:
    rows = read(path)["tasks"]
    if owner:
        rows = [t for t in rows if t["owner"] == owner.strip().lower()]
    if state:
        rows = [t for t in rows if t["state"] == state.strip().lower()]
    if room:
        rows = [t for t in rows if t["room"] == str(room).strip()]
    if open_only:
        rows = [t for t in rows if t["state"] in OPEN]
    # ⚠ PRIORITY BEFORE AGE. Higher priority first, then the old state/age order.
    # Every existing task has priority 0, so this is a no-op until an assigner
    # ranks something (see set_priority) — the ordering the heartbeat consumes.
    return sorted(rows, key=lambda t: (-_prio(t), _STATE_ORDER.get(t["state"], 9), t["created"]))


def pending_tasks(owner: str = "", room: str = "", path=None) -> list:
    """Actionable open tasks — the ARMING predicate. `blocked` is deliberately
    excluded (see PENDING): it is present but nobody can act on it, so it must
    not arm the heartbeat nor keep it alive on its own."""
    return [t for t in tasks(owner=owner, room=room, path=path) if t["state"] in PENDING]


def blocked_tasks(owner: str = "", room: str = "", path=None) -> list:
    """Open-but-stuck tasks. They surface in the retire reason and on the board;
    they never arm the heartbeat, or a blocked room would keep it ticking forever."""
    return [t for t in tasks(owner=owner, room=room, path=path) if t["state"] == "blocked"]


def history(task_id: str, path=None) -> list:
    tid = _norm_id(task_id)
    return [r for r in read(path)["log"] if r["id"] == tid]


def next_for(agent: str, path=None) -> dict | None:
    """The oldest `assigned` task for `agent`, or None.

    ⚠ ONLY `assigned` is deliverable. An `active` task is already in front of
    somebody, and a `proposed` one has not been decided — handing either out is
    how a queue starts inventing work."""
    agent = (agent or "").strip().lower()
    rows = [t for t in read(path)["tasks"]
            if t["owner"] == agent and t["state"] == DELIVERABLE]
    if not rows:
        return None
    return sorted(rows, key=lambda t: (-_prio(t), t["created"], t["id"]))[0]


def lead_view(lead: str, members=None, room: str = "", path=None) -> list:
    """The lead's plate, in drive order: its own open tasks first, then every
    member's, then unassigned work in the same room(s). the operator's rule: the lead
    is responsible for its own todo AND all todos in the room.

    Blocked tasks are INCLUDED — they must surface or they rot — but the state
    ordering (`_STATE_ORDER`) keeps them after `active` and before queued work,
    so they are visible without dominating the actionable list.

    `members` is the ordered list of the room's other agents, resolved by the
    WORKSPACE (which owns membership). Without it, "every member's" falls back
    to "every other owner sharing the lead's room(s)". `room` scopes to one room
    id; when omitted, the view spans every room the lead itself holds a task in.
    """
    lead = (lead or "").strip().lower()
    rows = [t for t in read(path)["tasks"] if t["state"] in OPEN]
    if room:
        room = str(room).strip()
        rows = [t for t in rows if t["room"] == room]
    else:
        my_rooms = {t["room"] for t in rows if t["owner"] == lead and t["room"]}
        if my_rooms:
            rows = [t for t in rows if not t["room"] or t["room"] in my_rooms]

    def key(t):
        owner_rank = 0 if t["owner"] == lead else 1
        if not t["owner"]:
            owner_rank = 2
        return (owner_rank, _STATE_ORDER.get(t["state"], 9), t["created"], t["id"])

    mine = [t for t in rows if t["owner"] == lead]
    if members:
        member_set = {str(m).strip().lower() for m in members}
        others = [t for t in rows if t["owner"] in member_set]
    else:
        others = [t for t in rows if t["owner"] and t["owner"] != lead]
    free = [t for t in rows if not t["owner"] and t["state"] == "proposed"]
    return sorted(mine, key=key) + sorted(others, key=key) + sorted(free, key=key)


# ══════════════════════════════════════════════════════════════════════════
# room seeding + room-scoped views — the Codex handoff surface
# ══════════════════════════════════════════════════════════════════════════

_SEED_SRC = "room-seed:"


def seed_room_tasks(project: str, room: str, items, actor: str, request_id: str,
                    path=None) -> list:
    """Idempotently seed a room's first todos. This is the registry half of
    Codex's `room_start` — Codex owns room creation; this call writes the room's
    initial TODO into the durable queue exactly once.

    ⚠ IDEMPOTENT ON request_id. The workspace post() dedups on request_id but the
    BMD registry has no such column, so the key is stamped into each seeded
    task's `source` as `room-seed:<hash(project,room,request_id)>`. A retry of the
    same request returns the already-seeded tasks instead of creating twins; a
    different request seeds again. `items` is a list of dicts, each carrying at
    least `title`, and optionally owner / owner_session / brief / acceptance /
    avoid. The room is the scope even when two rooms share a folder — the folder
    is Codex's concern, not the registry's.
    """
    room = str(room or "").strip()
    if not room:
        raise TaskError("seed_room_tasks needs a room id")
    if not request_id:
        raise TaskError("seed_room_tasks needs a request_id for idempotency")
    key = _SEED_SRC + hashlib.sha256(
        f"{project}:{room}:{request_id}".encode()).hexdigest()[:16]
    existing = [t for t in read(path)["tasks"] if t["source"] == key]
    if existing:
        return existing
    made = []
    for it in items:
        it = dict(it or {})
        made.append(propose(
            str(it.get("title") or "").strip() or "(untitled)",
            by=actor, owner=str(it.get("owner") or "").strip().lower(),
            brief=it.get("brief", ""), acceptance=it.get("acceptance", ""),
            avoid=it.get("avoid", ""), room=room,
            owner_session=str(it.get("owner_session") or "").strip(),
            source=key, path=path))
    return made


def _workspace_db(workspace_root=None) -> Path:
    base = Path(workspace_root or os.environ.get("LILJACK_WORKSPACE_ROOT",
                                                 str(Path.home() / ".cache/liljack/workspace")))
    return base / "workspace.sqlite3"


def _session_agent(session_id: str, workspace_root=None):
    """The AGENT behind a workspace session id, or None.

    ⚠ SESSIONS ARE EPHEMERAL; AGENTS ARE NOT. the operator restarts terminals, a
    harness crashes, lilJack respawns a pane — and each time the agent gets a
    NEW session id while its work keeps the old one (or none at all). Scoping a
    worker's todo view to a session id therefore orphans that worker's entire
    queue on every restart, which is what happened on 2026-09-10: a fresh codex
    session reported "own TODO view empty · no assigned task" while
    tui-visual-inspection-2 sat active and owned by `codex`.
    """
    import json as _json
    import sqlite3
    sid = str(session_id or "").strip()
    if not sid:
        return None
    db = _workspace_db(workspace_root)
    if not db.exists():
        return None
    try:
        con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        row = con.execute("SELECT data FROM sessions WHERE id=?", (sid,)).fetchone()
        con.close()
    except sqlite3.Error:
        return None
    if not row or not row[0]:
        return None
    try:
        agent = (_json.loads(row[0]) or {}).get("agent")
    except (ValueError, TypeError):
        return None
    agent = str(agent or "").strip().lower()
    return agent or None


def _room_team(room: str, workspace_root=None, strict: bool = False):
    """Read-only workspace lookup: (lead_session, member_sessions) for a room.

    Returns (None, []) when the workspace is absent or unreadable and `strict`
    is False — the registry never guesses membership. With `strict=True`, an
    absent workspace, an unknown room or an unreadable store RAISES, so a caller
    rendering a todo column gets a real error instead of an empty list that reads
    as "nothing to do".
    """
    import sqlite3
    db = _workspace_db(workspace_root)
    if not db.exists():
        if strict:
            raise ValueError("workspace unavailable")
        return None, []
    try:
        con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        if con.execute("SELECT 1 FROM rooms WHERE id=?", (room,)).fetchone() is None:
            con.close()
            if strict:
                raise ValueError("unknown room")
            return None, []
        members = [r[0] for r in con.execute(
            "SELECT session_id FROM room_members WHERE room_id=?", (room,))]
        role = {r[0]: r[1] for r in con.execute("SELECT session_id, role FROM team")}
        con.close()
    except sqlite3.Error as exc:
        if strict:
            raise ValueError(f"workspace unreadable: {type(exc).__name__}") from exc
        return None, []
    lead = next((s for s in members if role.get(s) == "lead"), None)
    return lead, members


def room_tasks(project: str, room: str, viewer_session: str, path=None,
               workspace_root=None, include_closed: bool = False,
               strict: bool = False) -> list:
    """Tasks in a room, scoped to the viewer — the durable-queue answer to "what
    is on my plate in this room".

    A LEAD sees its own tasks first, then every member's, then unassigned work in
    the room (the operator: the lead owns its todo AND all todos). A worker/reviewer
    sees ONLY its own — the lead's visibility must not duplicate ownership.
    THE OPERATOR (the operator, not a session) sees the WHOLE room, closed tasks
    included. Membership and role are resolved from the WORKSPACE.

    `include_closed` adds done/abandoned alongside open work. `strict=True`
    raises when the workspace or the room cannot be read — never an empty list
    that reads as "nothing to do". Without it the view falls back to own-only
    (never leaks another member's tasks on an unknown).
    """
    room = str(room or "").strip()
    viewer = str(viewer_session or "").strip()
    lead, _members = _room_team(room, workspace_root, strict=strict)
    states = STATES if include_closed else OPEN
    rows = [t for t in tasks(room=room, path=path) if t["state"] in states]
    if viewer == "operator":
        pass                          # the operator sees the whole room
    else:
        # A viewer is the lead only when the WORKSPACE says so; anything else —
        # worker, reviewer, or an unreadable workspace — is own-only.
        viewer_is_lead = bool(viewer and lead and viewer == lead)
        if not viewer_is_lead:
            # ⚠ A WORKER'S OWN WORK IS ITS AGENT'S WORK, NOT ITS SESSION'S. The
            # filter matched a session id against owner_session, or an agent
            # NAME against owner — so a viewer that is a session id holding a
            # task owned by an AGENT matched neither, and saw nothing. Every
            # terminal restart orphaned that worker's whole queue.
            # "Own-only" is preserved exactly: this widens the view to the
            # viewer's own agent and to nobody else's.
            # ⚠ AND THE WIDENING MUST NOT COLLAPSE ROLES. One agent can hold
            # TWO sessions in a room with different roles — codex as worker and
            # codex as reviewer is a real fixture — so matching on agent alone
            # let the worker read the reviewer's private task. A task BOUND to
            # a session that is still a member belongs to that session and
            # nobody else; the agent fallback applies only where the binding
            # cannot answer: no owner_session at all, or one naming a session
            # that is no longer in the room (the restart case this fixes).
            viewer_agent = _session_agent(viewer, workspace_root)
            present = set(_members or ())
            def _mine(t):
                if t["owner_session"] == viewer or t["owner"] == viewer:
                    return True
                if not viewer_agent or t["owner"] != viewer_agent:
                    return False
                bound = t["owner_session"]
                return (not bound) or (bound not in present)
            rows = [] if not viewer else [t for t in rows if _mine(t)]

    _va = _session_agent(viewer, workspace_root)
    _present = set(_members or ())

    def key(t):
        mine = (t["owner_session"] == viewer or t["owner"] == viewer
                or (_va and t["owner"] == _va
                    and (not t["owner_session"] or t["owner_session"] not in _present)))
        own = 0 if mine else 1
        if not t["owner"] and not t["owner_session"]:
            own = 2
        return (own, _STATE_ORDER.get(t["state"], 9), t["created"], t["id"])

    return sorted(rows, key=key)


# ══════════════════════════════════════════════════════════════════════════
# the handoff envelope — one shape, every agent, versioned
# ══════════════════════════════════════════════════════════════════════════

def envelope(task: dict, extra: str = "") -> str:
    """The prompt every agent receives for a task, in one shape.

    ⚠ THE RULE IS: render exactly the fields the task CARRIES — the same
    contract as this repo's artifact-view rule (a panel applies iff its
    artifacts are present). A task imported from a `TASK:` mail carries only a
    brief, because the human who wrote that mail put the acceptance and the
    boundaries in their own words inside it; the envelope then adds ONE
    identification line and nothing else. Padding a verbatim brief with empty
    headings is exactly the dilution the watcher must not perform.

    The header line is always present and always the same schema, so an agent
    (or a log reader) can identify a handoff without parsing the rest.
    """
    head = f"[lilJack task {ENVELOPE_VERSION}] {task.get('id','?')}"
    if task.get("mail"):
        head += f" (mail {task['mail']})"
    head += (f" · owner {task.get('owner') or 'unassigned'}"
             f" · by {task.get('by') or '?'}"
             f" · {task.get('title') or ''}").rstrip(" ·")
    parts = [head]
    for label, key in (("acceptance", "acceptance"), ("do not touch", "avoid"),
                       ("report", "report")):
        if task.get(key):
            parts.append(f"{label}: {task[key]}")
    if extra:
        parts.append(extra)
    brief = task.get("brief") or ""
    if brief:
        if len(parts) > 1:
            parts.append("---")
        return "\n".join(parts) + "\n" + brief
    return "\n".join(parts)


def digest(agent: str = "", path=None, budget: int = 700) -> str:
    """Standing-context block: what is on this agent's plate, then everyone
    else's, bounded. Returns "" when there is no registry."""
    try:
        store = read(path)
    except Exception:
        return ""
    if not store["exists"] or not store["tasks"]:
        return ""
    me = (agent or "").strip().lower()
    order = ([me] if me in AGENTS else []) + [a for a in AGENTS if a != me]
    lines = [f"[lilJack tasks] updated {store['updated'] or '?'} · "
             "durable task registry — task_list / task_update to change it"]
    for ag in order:
        mine = sorted([t for t in store["tasks"] if t["owner"] == ag and t["state"] in OPEN],
                      key=lambda t: _STATE_ORDER.get(t["state"], 9))
        label = f"you ({ag})" if ag == me else ag
        if not mine:
            lines.append(f"{label}: —")
            continue
        lines.append(label + ": " + " · ".join(
            f"{_STATE_MARK.get(t['state'], '?')} {t['id']}"
            + (f" {t['progress']}" if t["progress"] else "") for t in mine))
    free = [t["id"] for t in store["tasks"] if not t["owner"] and t["state"] == "proposed"]
    if free:
        lines.append("unassigned: " + " · ".join(free))
    text = "\n".join(lines)
    return text if len(text) <= budget else text[:budget - 1].rstrip() + "…"


# ══════════════════════════════════════════════════════════════════════════
# the legacy `TASK:` mail bridge
# ══════════════════════════════════════════════════════════════════════════

TASK_PREFIX = "TASK:"


def is_task_mail(msg) -> bool:
    """A deliverable task mail is one whose SUBJECT starts with `TASK:`. The
    subject decides — `TASK:` in a body is conversation about a task."""
    return str((msg or {}).get("subject") or "").lstrip().startswith(TASK_PREFIX)


def import_mail(agent: str = "codex", root=None, path=None, limit: int = 200,
                senders=None) -> list:
    """Record every `TASK:` mail for `agent` as a durable task, exactly once.

    ⚠ THIS IS THE REGRESSION FIX, and the important word is `unread_only=False`.
    The old queue read UNREAD mail, so a task Codex had already opened through
    `read_messages` was gone from it — which is how four live tasks became
    `idle_no_task`. Importing from the FULL mailbox means the record survives
    the read: whether the message is acked or not has no bearing on whether the
    work exists. Dedup is on the message id stamped into the task.

    Only a sender in ASSIGNERS is imported as ASSIGNED work; that is the same
    rule `assign()` enforces, applied at the bridge rather than bypassed by it.
    """
    who = (agent or "").strip().lower()
    ok_senders = tuple(senders) if senders is not None else ASSIGNERS
    try:
        box = mail.inbox(who, unread_only=False, limit=limit, root=root)
    except Exception:
        return []
    p = Path(path) if path else default_path()
    known = {t["mail"] for t in read(p)["tasks"] if t["mail"]}
    made = []
    for m in sorted(box, key=lambda m: (m.get("ts") or "", m.get("id") or "")):
        mid = m.get("id") or ""
        if not mid or mid in known or not is_task_mail(m):
            continue
        frm = (m.get("frm") or "").strip().lower()
        if frm not in ok_senders:
            continue
        subject = str(m.get("subject") or "").strip()
        title = subject[len(TASK_PREFIX):].strip() if is_task_mail(m) else subject
        row = propose(title or subject or mid, by=frm, owner=who,
                      brief=m.get("text") or "", report="",
                      source="mail", mail_id=mid, path=p)
        known.add(mid)
        made.append(row)
    return made


# ══════════════════════════════════════════════════════════════════════════
# board mirror — the injected digest must not disagree with the registry
# ══════════════════════════════════════════════════════════════════════════

_BOARD_STATE = {"proposed": "queued", "assigned": "queued", "active": "active",
                "blocked": "blocked", "done": "done"}


def sync_board(path=None, board_path=None) -> dict:
    """Mirror the registry onto the attention board.

    The board is what rides in every prompt; the registry is the truth. Two
    stores that can disagree is the class of bug this whole session is about,
    so the mirror runs one way only — registry → board, never back.
    """
    return _mirror_rows(read(path)["tasks"], board_path=board_path)


def _mirror_rows(rows, board_path=None) -> dict:
    """Push registry rows onto the board. One implementation, two callers.

    ⚠ WRITE ONLY WHAT CHANGED, OR THE MIRROR SILENCES THE HEARTBEAT. board.upsert
    stamps `updated = now()` unconditionally, and this function touches EVERY
    task. Once the mirror moved into _save() (423dc50) that meant a single
    registry write anywhere re-stamped all ~112 board rows, so every task looked
    advanced-seconds-ago. worker_wake builds its `fresh` set from those stamps,
    so nothing was ever stale and NOBODY WAS EVER NUDGED.

    Measured 2026-09-10: codex sat idle for 18 minutes on a task last touched 56
    minutes earlier, and the tick reported "no idle worker with fresh open work"
    because the board claimed every row was 30 seconds old. the operator saw it as codex
    hanging. It was my own mirror fix muting the heartbeat.

    A mirror's job is to make the board AGREE with the registry, not to declare
    activity. An unchanged row is not written, so its timestamp keeps meaning
    "when this task last actually moved".
    """
    out = {"updated": 0, "negated": 0, "skipped": 0}
    current = {}
    try:
        for it in board.read(Path(board_path) if board_path else board.default_path())["items"]:
            current[(it["agent"], it["task"])] = it
    except Exception:
        current = {}          # unreadable board -> write everything, as before
    for t in rows:
        if not t["owner"] or t["owner"] not in board.AGENTS:
            continue
        if t["state"] == "abandoned":
            board.negate(t["owner"], t["id"], _note(f"abandoned: {t['note']}"),
                         path=board_path)
            out["negated"] += 1
            continue
        want_state = _BOARD_STATE.get(t["state"], "queued")
        want_progress = t["progress"] or None
        want_note = (t["note"] or t["title"] or None)
        have = current.get((t["owner"], t["id"]))
        # ⚠ COMPARE WHAT THE BOARD WILL ACTUALLY STORE. upsert strips and clips
        # both fields, so comparing the registry's full text against the board's
        # clipped copy never matches and every long-noted row is rewritten on
        # every mirror — which is the same silencing bug, one layer down.
        # ⚠ …AND STRIP AGAIN AFTER CLIPPING. The clip can land mid-space, and
        # the board's own round-trip drops that trailing byte — so four rows
        # differed forever by exactly one space and were rewritten on every
        # mirror, re-stamping their timestamps and keeping them permanently
        # "fresh". A comparison has to model the full round-trip, not half of it.
        stored_progress = (None if want_progress is None
                           else str(want_progress).strip()[:board.MAX_PROGRESS].strip())
        stored_note = (None if want_note is None
                       else str(want_note).strip()[:board.MAX_NOTE].strip())
        if have is not None \
           and have.get("state") == want_state \
           and (stored_progress is None
                or (have.get("progress") or "").strip() == stored_progress) \
           and (stored_note is None
                or (have.get("note") or "").strip() == stored_note):
            out["skipped"] += 1
            continue
        board.upsert(t["owner"], t["id"], want_state, want_progress, want_note,
                     path=board_path)
        out["updated"] += 1
    return out


# ══════════════════════════════════════════════════════════════════════════
# CLI
# ══════════════════════════════════════════════════════════════════════════

def main(argv=None):
    import argparse
    import json
    ap = argparse.ArgumentParser(description="lilJack task registry (BMD)")
    ap.add_argument("--path", default="")
    ap.add_argument("--by", default="", help="the acting agent")
    sub = ap.add_subparsers(dest="cmd", required=True)
    ls = sub.add_parser("list"); ls.add_argument("--owner", default=""); ls.add_argument("--state", default="")
    ls.add_argument("--open", action="store_true"); ls.add_argument("--json", action="store_true")
    sh = sub.add_parser("show"); sh.add_argument("id")
    dg = sub.add_parser("digest"); dg.add_argument("--agent", default="")
    pr = sub.add_parser("propose"); pr.add_argument("title")
    pr.add_argument("--owner", default=""); pr.add_argument("--id", default="")
    pr.add_argument("--brief", default=""); pr.add_argument("--acceptance", default="")
    pr.add_argument("--avoid", default=""); pr.add_argument("--note", default="")
    pr.add_argument("--room", default="")
    pr.add_argument("--priority", type=int, default=0)
    asg = sub.add_parser("assign"); asg.add_argument("id"); asg.add_argument("owner")
    asg.add_argument("--note", default="")
    cl = sub.add_parser("claim"); cl.add_argument("id")
    st = sub.add_parser("state"); st.add_argument("id"); st.add_argument("state", choices=STATES)
    st.add_argument("--note", default=""); st.add_argument("--progress", default=None)
    pg = sub.add_parser("progress"); pg.add_argument("id"); pg.add_argument("progress")
    sp = sub.add_parser("set-priority"); sp.add_argument("id"); sp.add_argument("priority", type=int)
    un = sub.add_parser("unassign"); un.add_argument("id"); un.add_argument("--reason", default="")
    im = sub.add_parser("import-mail"); im.add_argument("--agent", default="codex")
    sub.add_parser("sync-board")
    a = ap.parse_args(argv)
    p = a.path or None
    who = a.by or mail.detect_agent()
    try:
        if a.cmd == "list":
            rows = tasks(a.owner, a.state, a.open, p)
            print(json.dumps(rows, indent=2, ensure_ascii=False) if a.json else
                  "\n".join(f"{_STATE_MARK.get(t['state'],'?')} {t['id']:<34} "
                            f"{t['state']:<10} {t['owner'] or '-':<8} "
                            f"{t['progress'] or '':<10} {t['title']}" for t in rows)
                  or "no tasks")
        elif a.cmd == "show":
            t = get(a.id, p)
            print(envelope(t))
            print("\nhistory:")
            for r in history(a.id, p):
                print(f"  {r['at']}  {r['op']:<9} {r['state']:<10} by {r['by']}  {r['note']}")
        elif a.cmd == "digest":
            print(digest(a.agent or who, p))
        elif a.cmd == "propose":
            print(json.dumps(propose(a.title, who, a.owner, a.brief, a.acceptance,
                                     a.avoid, a.note, task_id=a.id, room=a.room,
                                     priority=a.priority, path=p), ensure_ascii=False))
        elif a.cmd == "assign":
            print(json.dumps(assign(a.id, a.owner, who, a.note, p), ensure_ascii=False))
        elif a.cmd == "claim":
            print(json.dumps(claim(a.id, who, path=p), ensure_ascii=False))
        elif a.cmd == "state":
            print(json.dumps(set_state(a.id, a.state, who, a.note, a.progress, p), ensure_ascii=False))
        elif a.cmd == "progress":
            print(json.dumps(set_progress(a.id, a.progress, who, path=p), ensure_ascii=False))
        elif a.cmd == "set-priority":
            print(json.dumps(set_priority(a.id, a.priority, who, path=p), ensure_ascii=False))
        elif a.cmd == "unassign":
            print(json.dumps(unassign(a.id, who, a.reason, p), ensure_ascii=False))
        elif a.cmd == "import-mail":
            made = import_mail(a.agent, path=p)
            print(f"imported {len(made)}: " + ", ".join(t["id"] for t in made))
        elif a.cmd == "sync-board":
            print(json.dumps(sync_board(p), ensure_ascii=False))
    except (Denied, TaskError) as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
