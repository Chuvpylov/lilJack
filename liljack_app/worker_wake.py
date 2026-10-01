"""Wake an idle WORKER that still has open work.

The room heartbeat wakes the LEAD when a notice is queued. Nothing woke the
workers, so after every turn they sat idle until the lead hand-poked them — which
made the lead the bottleneck and looked, from outside, like everyone was asleep.

The guards matter more than the feature:

⚠ IDLE-GATED. Never send to a busy or unknown worker. Same asymmetry as the lead
  pump: typing into a working agent corrupts its turn; waiting costs one cycle.
⚠ DEDUPED PER TASK. A worker is nudged about a given task at most once per
  COOLDOWN_S. Without this the loop re-sends the same item every tick and turns
  a helper into a barrage.
⚠ THE LEAD IS INCLUDED. It was excluded at first to avoid fighting the room
  pump, but that pump only fires on queued NOTICES — so a lead sitting idle with
  its own open work was the one agent nothing could start. the operator's point: the
  heartbeat should spank everyone.
⚠ ONE WORKER PER TICK, so a burst of idleness cannot become a burst of typing.

⚠ TMUX IS THE TRUTH FOR LIVENESS (2026-09-10). candidates() used to trust the
  sessions ROW and only skipped state=='stopped', so it kept picking a session
  whose tmux pane was long gone; the CLI answered "Target has no live terminal",
  nudge() threw the reason away, and record() ran only on success — so an
  unreachable agent was retried forever, silently, while everyone else waited.
  That was the whole of "the heartbeat spanks nobody". Now liveness comes from
  Tmux.sessions(), a failed send returns its REASON, and every attempt — success
  or failure — records its backoff, so failures cannot spin.

⚠ THE ESCALATION LADDER NEVER DROPS A RUNG (the operator's protocol). Three unanswered
  nudges on a todo escalate to the room LEAD naming the owner and the count. An
  agent with no live terminal, or whose send reported an API/credit/authentication
  failure, is marked UNAVAILABLE with the reason and escalated at once, not
  retried. The lead gets ONE batched digest per tick, not one wake per event; a
  lead that stays silent through its own three strikes is escalated to THE OPERATOR by
  mail. "Cannot tell" waits rather than firing.
"""
from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import time
from pathlib import Path

COOLDOWN_S = 15 * 60
# A task nobody has touched for this long is not "in progress", it is STUCK, and
# poking its owner again has already failed once. the operator's rule: corresponding
# tasks go to their owner; STALE tasks escalate to the room's lead.
STALE_TASK_S = 45 * 60

# ⚠ Do not poke an owner about work they are demonstrably ON. The ordinary
# nudge fired for ANY open task of an idle agent regardless of how recently the
# owner had advanced it — staleness only ever gated the ESCALATION path. So a
# long-running task with an owner reporting progress every few minutes still
# drew a nudge every cooldown: `trio-relaunch` monitors a 62-hour training run,
# which at a 5-minute cadence is roughly 750 nudges for work that is going fine.
# An owner who advanced a task inside this window has already answered the
# question the nudge asks.
FRESH_TASK_S = 20 * 60

# A strike is an unanswered nudge attempt. At this many, the todo escalates.
STRIKES_ESCALATE = 3
# How long the loop leaves an escalated-to-operator room alone before it may mail
# the human again (the operator must not become a nudge target).
OPERATOR_WINDOW_S = 6 * 3600
# A failed send with one of these is FATAL for the agent: mark it unavailable and
# escalate, rather than backing off and retrying forever.
FATAL_REASONS = ("no live terminal", "credit", "quota", "billing",
                 "authentication", "unauthorized", "api key")

# ⚠ FATAL SAYS "STOP NUDGING NOW"; IT DOES NOT SAY "FOREVER". Exactly one of
# those reasons ends in a way we can OBSERVE: a terminal that was missing comes
# back, and its pane is right there in tmux. Credit, quota, billing and auth are
# not fixed by a pane existing, so they stay stuck until a human clears them.
# Conflating the two is what removed deepseek from the heartbeat for 11.5 hours.
RECOVERABLE_REASONS = ("no live terminal",)

STATE = Path(os.environ.get("LILJACK_WAKE_STATE",
                            str(Path.home() / ".cache/liljack/worker_wake.json")))

__all__ = ["candidates", "pick", "record", "nudge", "nudge_text", "context_line", "tick", "question_tick",
           "COOLDOWN_S", "strikes", "last_reason", "mark_unavailable", "unavailable",
           "clear_unavailable", "STRIKES_ESCALATE", "RECOVERABLE_REASONS", "rank_tasks",
           "room_lead", "digest_signature", "remember_digest", "room_allows", "reset_room_cache"]


def _workspace(root=None) -> Path:
    base = Path(root or os.environ.get("LILJACK_WORKSPACE_ROOT",
                                       str(Path.home() / ".cache/liljack/workspace")))
    return base / "workspace.sqlite3"


def _state() -> dict:
    try:
        return json.loads(STATE.read_text())
    except Exception:
        return {}


def _write_state(s: dict) -> None:
    try:
        STATE.parent.mkdir(parents=True, exist_ok=True)
        STATE.write_text(json.dumps(s))
    except OSError:
        pass                      # a lost memo means one duplicate, not a crash


def backoff_for(n: int) -> float:
    """Cooldown after `n` consecutive nudges about the same task.

    ⚠ A fixed cooldown treats "poked once" and "poked twenty times" alike. A
    long-running task cannot be finished on demand — `trio-relaunch` monitors a
    62-hour training run — so a flat 15-minute cooldown against a 20-minute
    freshness window puts its owner on a treadmill: report, get poked, report,
    get poked, ~186 times. Worse, the two windows ALIAS: measured 2026-09-09,
    progress at 07:52:06 and a nudge at 08:12:09 is exactly 1200s, so the
    `age < FRESH_TASK_S` test failed by a single second, twice running.

    Doubling per repeat (15m, 30m, 60m, capped at 2h) keeps the FIRST nudge
    prompt — which is the one that catches real stalls — while a task that is
    simply long stops being shouted at. The count RESETS when the task stops
    being nudged (see `record`).
    """
    return min(COOLDOWN_S * (2 ** max(0, n - 1)), 2 * 60 * 60)


def record(session_id: str, task: str, now=None, reason: str = "", delivered: bool = True) -> None:
    """Remember an ATTEMPT — success or failure — so the same task is not
    re-sent next tick, and so repeated failures count toward escalation.

    ⚠ A STRIKE IS A DELIVERED NUDGE THE AGENT IGNORED. A send the HARNESS refused
    (delivered=False: validator, no pane, …) is our fault, not the agent's, and
    must not count: on 2026-09-12 sixteen refused digests ("control characters")
    were counted as strikes against the lead, so every later DELIVERED digest
    also escalated to the operator — the heartbeat "spanked the operator" instead of the team."""
    s = _state()
    key = f"{session_id}:{task}"
    prev = s.get(key, 0)
    now = now or time.time()
    # Consecutive = nudged again within the window this backoff already granted.
    n = int(s.get(key + ":n", 0))
    if delivered:
        n = n + 1 if prev and (now - prev) < backoff_for(n) * 3 else 1
    s[key + ":n"] = n
    s[key] = now
    if reason:
        s[key + ":reason"] = str(reason)[:400]
    _write_state(s)


def strikes(session_id: str, task: str) -> int:
    """Consecutive unanswered attempts for this (session, task)."""
    return int(_state().get(f"{session_id}:{task}:n", 0))


def last_reason(session_id: str, task: str) -> str:
    return str(_state().get(f"{session_id}:{task}:reason", ""))


def mark_unavailable(session_id: str, reason: str, now=None) -> None:
    """Record that an agent is unreachable, WITH the reason. Never retried."""
    s = _state()
    s[f"unavail:{session_id}"] = {"reason": str(reason or "")[:400],
                                  "at": now or time.time()}
    _write_state(s)


def unavailable(session_id: str):
    """The unavailable record for a session, or None."""
    return _state().get(f"unavail:{session_id}")


def clear_unavailable(session_id: str) -> None:
    s = _state()
    if s.pop(f"unavail:{session_id}", None) is not None:
        _write_state(s)


def _is_fatal(reason: str) -> bool:
    r = (reason or "").lower()
    return any(k in r for k in FATAL_REASONS)


def _is_recoverable(reason: str) -> bool:
    """True when the condition's END is directly observable (see the constant)."""
    r = (reason or "").lower()
    return any(k in r for k in RECOVERABLE_REASONS)


def candidates(project, root=None, live=None) -> list:
    """Every app-managed session of this project, LEAD INCLUDED.

    [{id, agent, harness, role}] — role is carried so a caller can word the nudge
    differently, not so it can skip anyone.

    ⚠ TMUX IS THE TRUTH. When `live` (an iterable of tmux-live session ids) is
    given, a candidate must be IN it; the DB row's state is not trusted for
    liveness. When `live` is None the old DB-row filter (state != 'stopped')
    applies, which keeps callers that have no tmux view working.
    """
    db = _workspace(root)
    if not db.exists():
        return []
    try:
        con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        lead = con.execute("SELECT session_id FROM team WHERE role='lead'").fetchone()
        lead = lead[0] if lead else None
        rows = con.execute("SELECT id, data FROM sessions").fetchall()
        con.close()
    except sqlite3.Error:
        return []
    live_ids = set(live) if live is not None else None
    out = []
    for sid, blob in rows:
        try:
            d = json.loads(blob)
        except Exception:
            continue
        if d.get("harness") != "tmux":
            continue
        if live_ids is not None:
            if sid not in live_ids:
                continue              # tmux says it is gone — not a target
        elif d.get("state") == "stopped":
            continue
        if d.get("project") != str(project):
            continue
        harness = {"deepseek": "opencode", "codex": "codex", "claude": "claude"}.get(d.get("agent"))
        if not harness:
            continue
        out.append({"id": sid, "agent": d.get("agent"), "harness": harness,
                    "role": "lead" if sid == lead else "worker"})
    return out


def room_lead(project, root, room: str):
    """The exact session that leads ROOM (team.role == 'lead' among the room's
    room_members), or None. ⚠ THE ROOM LEAD IS NOT THE TEAM-TABLE LEAD. Measured
    2026-09-12: team.role='lead' named s-ca1afb3b (an old claude session) while
    room r-22d217a2's lead was s-ddd6cac7, so every gated/blocked digest went to
    the wrong session and the operator's lilJack queue sat silent (heartbeat-room-lead-routing)."""
    if not room:
        return None
    db = _workspace(root)
    if not db.exists():
        return None
    try:
        con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
        # Roles live in the TEAM table (one row per session; several leads may
        # exist, one per room). Membership lives in room_members. The lead of
        # THIS room is the member whose team role is lead and who is not stopped.
        rows = con.execute("SELECT m.session_id, s.data FROM room_members m "
                           "JOIN team t ON t.session_id=m.session_id "
                           "LEFT JOIN sessions s ON s.id=m.session_id "
                           "WHERE m.room_id=? AND t.role='lead'", (room,)).fetchall()
        con.close()
    except sqlite3.Error:
        return None
    for sid, blob in rows:
        try:
            d = json.loads(blob) if blob else {}
        except Exception:
            d = {}
        if d.get("state") != "stopped":
            return sid
    return None


_ROOM_CACHE = {"members": {}, "session": {}}


def _room_tables(root):
    """(room -> set(session ids), session -> room) from room_members, cached per tick."""
    if _ROOM_CACHE["members"] or _ROOM_CACHE["session"]:
        return _ROOM_CACHE["members"], _ROOM_CACHE["session"]
    db = _workspace(root)
    members, sess = {}, {}
    if db.exists():
        try:
            con = sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)
            for sid, rid in con.execute("SELECT session_id, room_id FROM room_members"):
                members.setdefault(rid, set()).add(sid)
                sess[sid] = rid
            con.close()
        except sqlite3.Error:
            pass
    _ROOM_CACHE["members"], _ROOM_CACHE["session"] = members, sess
    return members, sess


def reset_room_cache():
    _ROOM_CACHE["members"], _ROOM_CACHE["session"] = {}, {}


def room_allows(root, session_id: str, task_room: str) -> bool:
    """⚠ A ROOM TASK ONLY REACHES THAT ROOM'S MEMBERS, AND A ROOM-LESS TASK NEVER
    LANDS IN A ROOM. Measured 2026-09-12 (fix-cross-room-session-split): the trio
    room's codex and deepseek were nudged within 60 s of joining about TUI-room
    tasks, one codex session then served both rooms at once and went silent."""
    members, sess = _room_tables(root)
    if task_room:
        return session_id in members.get(task_room, set())
    return not sess.get(session_id)


def digest_signature(escalations) -> str:
    """Stable id of WHAT is being escalated. A NEW item (a worker just blocked a
    task, a room just gated one) bypasses the digest cooldown: the operator's rule —
    the lead is bugged for new actions, never for nothing."""
    items = sorted((str(e.get("owner")), str(e.get("task")), str(e.get("reason"))[:80])
                   for e in (escalations or []))
    return hashlib.sha256(json.dumps(items).encode()).hexdigest()[:16]


def remember_digest(session_id: str, signature: str, now=None) -> None:
    s = _state()
    s[f"{session_id}:digest:sig"] = signature
    s[f"{session_id}:digest"] = now or time.time()
    _write_state(s)


def rank_tasks(open_items: dict, prio: dict) -> dict:
    """Order each owner's tasks by DESCENDING priority, in place; return it.

    ⚠ RANK, DON'T JUST AGE. `open_items` arrives in board/registry order, which
    is state then age — so the OLDEST eligible task was handed out first and a
    parked item could jump the queue. The sort is STABLE, so equal priority keeps
    that order; every task without a rank is 0, which is the old behaviour.
    """
    for owner in open_items:
        open_items[owner].sort(key=lambda tid: -prio.get(tid, 0))
    return open_items


def _digest_text(escalations) -> str:
    lines = ["lilJack heartbeat: the following work needs you (lead)."]
    for e in escalations:
        owner = e.get("owner") or "?"
        task = e.get("task")
        reason = e.get("reason") or ""
        if task:
            line = f"- {task} (owner {owner})"
            if e.get("room"):
                line += f" [room {e['room']}]"
            if e.get("strikes"):
                line += f" — {e['strikes']} unanswered nudges"
            elif reason:
                line += f" — {reason}"
        else:
            line = f"- {owner} is unavailable — {reason}"
        lines.append(line)
    lines.append("Drive it, reassign it, or report in the room.")
    # ⚠ ONE LINE. The CLI's `send` stage rejects control characters ("Send stages
    # one line"), so a multi-line digest was refused for EVERY escalation and no
    # lead nudge went out from 20:31 EDT on 2026-09-12 — the heartbeat "stopped
    # spanking everyone". Join with a separator and squash any control byte.
    text = " · ".join(lines)
    return "".join(ch if (ord(ch) >= 32 and not 127 <= ord(ch) < 160) else " " for ch in text)


def pick(project, lookup, open_items, root=None, now=None, stale=None, fresh=None,
         live=None, gated=None, blocked=None, owner_sessions=None, task_rooms=None,
         task_states=None):
    """The single session to nudge this tick, or None.

    `open_items` maps agent -> [task] for that agent's OWN work. `stale` is a
    list of (owner, task) nobody has advanced; `fresh` is the set of tasks
    advanced within FRESH_TASK_S. `live` is the tmux-live session id set — the
    liveness truth (see candidates()). `gated` is a list of {owner, task, reason}
    for tasks a room phase or an open question blocks from nudging — they are
    surfaced in the lead's digest, never nudged to their owner.

    ⚠ A TASK'S EXACT SESSION WINS OVER THE AGENT SLOT. `owner_sessions` maps
    task id -> the session the registry names as owner. When that session is one
    of the live candidates, ONLY it may be nudged about the task; the agent slot
    is a fallback for tasks with no session (or a session that is gone). Without
    this, two sessions of one provider were interchangeable and the heartbeat
    nudged the wrong one (visual-inspection-round-2 → s-3099f65f was sent to
    s-9ce113bb; both deepseek).

    Returns either an OWNER nudge ({session,agent,task,role,kind:"owner"}) or an
    ESCALATION to the lead ({session,agent,role:"lead",kind:"escalate",
    escalations:[{owner,task,reason,strikes,unavailable}]}), which batches every
    pending escalation into ONE digest so the lead is woken once per tick, not
    once per event.
    """
    fresh = fresh or set()
    now = now or time.time()
    seen = _state()
    people = candidates(project, root, live=live)
    lead = next((p for p in people if p.get("role") == "lead"), None)
    owner_sessions = owner_sessions or {}
    task_rooms = task_rooms or {}
    task_states = task_states or {}
    by_id = {p["id"]: p for p in people}

    escalations = []

    # unavailable agents — escalate once, never retry. These are collected over
    # the DB-known candidates (NOT the live-filtered `people`), so an agent whose
    # terminal just died is still escalated rather than vanishing behind the
    # liveness filter that rightly excludes it from ordinary nudges.
    for w in candidates(project, root):
        if w.get("role") == "lead":
            continue
        u = unavailable(w["id"])
        if u:
            escalations.append({"owner": w["agent"], "task": None,
                                "reason": u.get("reason") or "unavailable",
                                "unavailable": True})

    # ⚠ A WEDGED SESSION MUST REACH THE LEAD, OR IT IS SILENT FOREVER. An
    # UNKNOWN session is rightly never nudged (it might genuinely be mid-call),
    # and the ordinary loop below simply skips it — so a session whose events
    # stopped arriving 14 hours ago produces no nudge, no escalation and no
    # log line. codex sat like that all night holding an open task, and the
    # only thing that noticed was a human reading ages by hand.
    #
    # Reported, never nudged: the lead is told, the wedged agent is left alone.
    for w in people:
        if w.get("role") == "lead":
            continue
        st = lookup(w["id"], project, w["harness"]) or {}
        if st.get("reason_code") != "event_stale":
            continue
        if not (open_items.get(w["agent"]) or []):
            continue              # wedged with nothing open is not urgent
        escalations.append({"owner": w["agent"], "task": None,
                            "reason": st.get("reason") or "session appears wedged",
                            "wedged": True})

    # struck-out tasks — 3 unanswered nudges
    for w in people:
        if w.get("role") == "lead":
            continue
        for task in (open_items.get(w["agent"]) or []):
            n = strikes(w["id"], task)
            if n >= STRIKES_ESCALATE:
                escalations.append({"owner": w["agent"], "task": task,
                                    "reason": last_reason(w["id"], task) or "no response",
                                    "strikes": n})

    # stale tasks — nobody advanced them
    for owner, task in (stale or []):
        if lead and owner == lead["agent"]:
            continue               # the lead's own stale task is not escalated to it
        escalations.append({"owner": owner, "task": task, "reason": "stale"})

    # gated tasks — room phase / open question blocks them; they must not vanish
    for g in (gated or []):
        escalations.append({"owner": g["owner"], "task": g["task"],
                            "reason": g["reason"]})

    # blocked registry tasks — surfaced to the lead so the queue is resolved, not
    # nudged to their owner (nobody can act on a blocked task).
    for b in (blocked or []):
        escalations.append({"owner": b["owner"], "task": b["task"],
                            "reason": b["reason"]})

    # ⚠ ROUTE EACH ESCALATION TO ITS ROOM'S LEAD. A room-stamped task belongs to
    # the room lead named in the room store; the team-table lead is only the
    # fallback for standalone tasks or rooms whose lead is not live. A digest
    # with a NEW item is sent at once (signature changed); an unchanged digest
    # waits COOLDOWN_S so the lead is not hammered for nothing.
    if escalations:
        groups = {}            # lead session id -> [escalations]
        for e in escalations:
            target = None
            room = task_rooms.get(e.get("task") or "")
            if room:
                sid = room_lead(project, root, room)
                if sid and sid in by_id:
                    target = by_id[sid]
            if target is None:
                target = lead
            if target is None:
                continue
            groups.setdefault(target["id"], (target, []))[1].append({**e, "room": room or ""})
        for sid, (target, items) in groups.items():
            state = lookup(sid, project, target["harness"])
            if not (state and state.get("state") == "idle"):
                continue
            sig = digest_signature(items)
            key = f"{sid}:digest"
            prev_sig = seen.get(key + ":sig")
            changed = prev_sig is not None and sig != prev_sig   # a digest never signed is not "new"
            if changed or now - seen.get(key, 0) >= COOLDOWN_S:
                return {"session": sid, "agent": target["agent"], "role": "lead",
                        "kind": "escalate", "escalations": items, "signature": sig}

    # ordinary owner nudges, lead included for its own work
    for w in people:
        if unavailable(w["id"]):
            continue               # an unavailable agent is never retried
        tasks = open_items.get(w["agent"]) or []
        if not tasks:
            continue
        state = lookup(w["id"], project, w["harness"])
        if not state or state.get("state") != "idle":
            continue               # busy, or unknowable: leave it alone
        for task in tasks:
            # ⚠ EXACT SESSION OVER AGENT SLOT (see owner_sessions above). If the
            # registry names a DIFFERENT live session as owner, this candidate is
            # the wrong one and must not be nudged about it. A task whose named
            # session is not live falls through to the agent slot as before.
            want = (owner_sessions.get(task) or "").strip()
            if want and want in by_id and want != w["id"]:
                continue
            # ⚠ ROOM SCOPE (fix-cross-room-session-split). A task stamped with a
            # room reaches only that room's members; a task with no room never
            # reaches a session that sits in a room.
            if not room_allows(root, w["id"], task_rooms.get(task, "")):
                continue
            # ⚠ AN ACTIVE TASK NEVER NUDGES A SECOND SESSION OF ITS AGENT. Once a
            # session started it, only that session is asked about it; if the
            # registry does not know which session (legacy row), nobody is.
            if task_states.get(task) == "active" and want != w["id"]:
                continue
            if task in fresh:
                continue           # owner advanced it recently — nothing to ask
            if strikes(w["id"], task) >= STRIKES_ESCALATE:
                continue           # already escalated; do not re-nudge the owner
            key = f'{w["id"]}:{task}'
            if now - seen.get(key, 0) < backoff_for(int(seen.get(key + ":n", 0))):
                continue
            return {"session": w["id"], "agent": w["agent"], "task": task,
                    "role": w.get("role", "worker"), "kind": "owner",
                    "room": task_rooms.get(task, "")}
    return None


def context_line(room: str) -> str:
    """One line that lets a compacted or restarted agent find its room again
    (context-reinjection, 2026-09-13): the room id and the exact command."""
    room = (room or "").strip()
    if not room:
        return ""
    return (f" Room {room} — re-read context: python3 -m liljack_app.cli room"
            f" --root $LILJACK_WORKSPACE_ROOT --project $LILJACK_WORKSPACE_PROJECT --context --to {room}")


def nudge_text(choice) -> str:
    """The nudge body (one line, no control characters — the CLI send stage
    rejects them). Owner nudges and lead digests both carry the room id."""
    if choice.get("kind") == "escalate":
        text = _digest_text(choice["escalations"])
    else:
        text = (f"lilJack heartbeat: you are idle and '{choice['task']}' is still open in this "
                f"room. Pick it up and report in the room when done."
                + (" You are the lead — drive the others too." if choice.get("role") == "lead" else "")
                + context_line(choice.get("room", "")))
    return "".join(ch if (ord(ch) >= 32 and not 127 <= ord(ch) < 160) else " " for ch in text)


def _cli_error(r) -> str:
    """Extract the CLI's {"error": ...} from a subprocess result."""
    for stream in (getattr(r, "stderr", ""), getattr(r, "stdout", "")):
        if not stream:
            continue
        try:
            j = json.loads(stream.strip())
        except Exception:
            continue
        if isinstance(j, dict) and j.get("error"):
            return str(j["error"])[:400]
    return ""


def nudge(project, choice, text=None) -> dict:
    """Stage one line in the worker's prompt and press Enter. Operator identity.

    Two calls on purpose, exactly as the CLI defines them: `send` stages, `submit`
    commits. Anything that types into another agent should be visible as two
    deliberate steps, not one hidden one.

    ⚠ The result always carries `reason` — the refusal text when a send or submit
    failed, "" on success — and NEVER calls record() itself; the caller records
    the attempt (success OR failure) so failures cannot spin without a backoff.
    """
    import subprocess
    repo = Path(__file__).resolve().parents[1]
    env = dict(os.environ)
    env.pop("LILJACK_WORKSPACE_SESSION", None)      # act as the operator, not as a peer
    env["LILJACK_AGENT"] = "operator"
    if choice.get('workspace_root'):
        env['LILJACK_WORKSPACE_ROOT'] = str(choice['workspace_root'])
    env["PYTHONPATH"] = str(repo) + os.pathsep + str(repo / "toolbox") + (
        os.pathsep + str(Path(os.environ["LILJACK_TOOLBOX_ROOT"]) / "toolbox") if os.environ.get("LILJACK_TOOLBOX_ROOT") else "")
    if text is None:
        text = nudge_text(choice)
    base = ["python3", "-m", "liljack_app.cli", "session"]
    out = {"agent": choice["agent"], "task": choice.get("task"), "session": choice.get("session"),
           "kind": choice.get("kind", "owner"), "sent": False, "submitted": False, "reason": ""}
    try:
        r = subprocess.run(base + ["send", "--session", choice["session"], "--project", str(project),
                                   "--text", text], cwd=str(repo / "toolbox"), env=env,
                           capture_output=True, stdin=subprocess.DEVNULL, text=True, timeout=45)
        err = _cli_error(r)
        if r.returncode != 0 or err:
            out["reason"] = err or f"send failed (rc {r.returncode})"
            return out
        out["sent"] = True
        r = subprocess.run(base + ["submit", "--session", choice["session"],
                                   "--project", str(project)], cwd=str(repo / "toolbox"),
                           env=env, capture_output=True, stdin=subprocess.DEVNULL,
                           text=True, timeout=45)
        err = _cli_error(r)
        if r.returncode != 0 or err:
            out["reason"] = err or f"submit failed (rc {r.returncode})"
            return out
        out["submitted"] = True
    except (OSError, subprocess.SubprocessError) as exc:
        out["reason"] = f"{type(exc).__name__}: {exc}"
    return out


def _default_send_mail(text: str, to: str, subject: str = "", root=None) -> bool:
    try:
        import liljack_mail as mail
        mail.send(text, to, frm="codex", subject=subject[:mail.MAX_SUBJECT],
                  project="demo", root=root)
        return True
    except Exception:
        return False


def _escalate_operator(lead_agent, escalations, send_mail, root=None, now=None) -> bool:
    """Mail the operator that the room lead is unresponsive. Deduped by a long window."""
    s = _state()
    now = now or time.time()
    if now - s.get("operator:last", 0) < OPERATOR_WINDOW_S:
        return False
    s["operator:last"] = now
    _write_state(s)
    lines = [f"lilJack heartbeat: the room lead ({lead_agent}) has not responded "
             f"to {STRIKES_ESCALATE} nudges."]
    for e in escalations:
        lines.append(f"- {e.get('owner') or '?'}: {e.get('task') or 'unavailable'} — {e.get('reason') or ''}")
    fn = send_mail or _default_send_mail
    try:
        fn("\n".join(lines), "operator", subject="HEARTBEAT: room lead unresponsive", root=root)
        return True
    except Exception:
        return False


def blocked_post_tick(project, root, live, lookup, *, dry_run=True):
    """Notify exact blocked owners of new lead posts; never unblock their tasks.

    Messages remain in the room store. A cursor/cooldown in the existing wake
    state is enough; no second delivery queue. Claim before transport so a
    timeout or process death cannot replay a possibly submitted prompt.
    """
    import fcntl
    from contextlib import closing
    from datetime import datetime
    import liljack_tasks

    path = _workspace(root)
    if not path.exists() or (path.parent / 'room-delivery.stop').exists():
        return None
    tasks = [t for t in (liljack_tasks.tasks(open_only=True) or [])
             if t.get('state') == 'blocked' and t.get('room') and t.get('owner_session')]
    if not tasks:
        return None
    people = {p['id']: p for p in candidates(project, root, live=live)}
    # Share the room transport dispatch lock, including across UI/heartbeat callers.
    with (path.parent / 'room-delivery.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return None
        with closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)) as db:
            seen = json.loads(STATE.read_text()) if STATE.exists() else {}
            if not isinstance(seen, dict):
                raise ValueError('invalid worker wake state')
            now = time.time()
            choices = []
            for task in tasks:
                sid, room = task['owner_session'], task['room']
                person = people.get(sid)
                if not person or person['agent'] != task.get('owner') or unavailable(sid):
                    continue
                try:
                    blocked_at = datetime.fromisoformat(task['updated'].replace('Z', '+00:00'))
                    if blocked_at.tzinfo is None:
                        continue
                except (KeyError, TypeError, ValueError):
                    continue
                # Do not transfer a room notification to a moved or replacement session.
                if not db.execute('SELECT 1 FROM room_members rm JOIN rooms r ON r.id=rm.room_id '
                                  "WHERE rm.session_id=? AND r.id=? AND r.project=? AND r.state='active'",
                                  (sid, room, str(project))).fetchone():
                    continue
                leads = db.execute('SELECT t.session_id FROM team t JOIN room_members rm '
                                   "ON rm.session_id=t.session_id WHERE rm.room_id=? AND t.role='lead'",
                                   (room,)).fetchall()
                if len(leads) != 1 or leads[0][0] == sid:
                    continue
                key = f'blocked-post:{project}:{room}:{sid}'
                memo = seen.get(key, {})
                # An uncertain attempt consumes THAT post, not this member's
                # future notifications. The seq predicate below prevents replay;
                # a newer lead post may pass once the normal cooldown expires.
                if now - memo.get('at', 0) < COOLDOWN_S:
                    continue
                # Only posts after this block, addressed to this exact member.
                post = db.execute('SELECT m.seq,m.id FROM messages m JOIN message_recipients mr '
                                  'ON mr.message_id=m.id WHERE m.project=? AND m.destination=? '
                                  'AND m.sender=? AND mr.recipient=? AND m.seq>? '
                                  'AND julianday(m.created_at)>julianday(?) ORDER BY m.seq LIMIT 1',
                                  (str(project), room, leads[0][0], sid, memo.get('seq', 0),
                                   blocked_at.isoformat())).fetchone()
                if post:
                    choices.append((post[0], sid, post[1], room, key, person))
            for seq, sid, mid, room, key, person in sorted(choices):
                state = lookup(sid, project, person['harness'])
                if not state or state.get('state') != 'idle':
                    continue
                fresh = lookup(sid, project, person['harness'])
                if not fresh or fresh.get('state') != 'idle' or any(
                        fresh.get(k) != state.get(k) for k in ('native', 'at', 'event')):
                    continue
                choice = dict(person, session=sid, room=room, kind='blocked-post', message=mid,
                              workspace_root=str(path.parent))
                if dry_run:
                    return {'action': 'worker-dry-run', 'detail': 'would notify blocked owner of lead post',
                            'session': sid, 'message': mid}
                # Unlike the legacy best-effort state writer, failure to persist
                # this claim must prevent sending. Atomic replace avoids a torn cursor.
                seen[key] = {'seq': seq, 'at': now, 'outcome': 'uncertain'}
                STATE.parent.mkdir(parents=True, exist_ok=True)
                tmp = STATE.with_name(STATE.name + '.blocked-post.tmp')
                tmp.write_text(json.dumps(seen))
                tmp.replace(STATE)
                text = (f'lilJack room update: your lead posted {mid} in room {room}. '
                        'Read that post and reassess your blocked task; it remains blocked until '
                        'you update it. Follow the room phase. '
                        f'Read: ./liljack room --message {mid}' + context_line(room))
                try:
                    result = nudge(project, choice, text)
                    outcome = 'submitted' if result.get('submitted') else 'uncertain'
                except Exception:
                    outcome = 'uncertain'  # possible partial submission: never auto-replay
                seen = json.loads(STATE.read_text())
                seen[key] = {'seq': seq, 'at': now, 'outcome': outcome}
                tmp.write_text(json.dumps(seen))
                tmp.replace(STATE)
                return {'action': 'worker-blocked-post', 'session': sid, 'message': mid,
                        'outcome': outcome}
    return None


def tick(project, root=None, dry_run=True, send_mail=None, tmux=None) -> dict:
    """One worker-wake pass: at most ONE nudge, and none at all when unsure."""
    import collections
    from datetime import datetime
    try:
        import lead_lifecycle
        import review_panel
    except Exception as exc:
        return {"action": "worker-skip", "detail": type(exc).__name__}

    scope = review_panel.room_scope()
    opens = collections.defaultdict(list)
    for t in review_panel.todos(since=scope):
        if t["state"] in ("active", "queued"):
            opens[t["agent"]].append(t["task"])
    # ⚠ THE REGISTRY IS THE QUEUE; THE BOARD IS A VIEW OF IT. The nudge set was
    # built from the BOARD alone, whose vocabulary is active/queued — so a
    # registry task sitting at ASSIGNED, which is work handed out and never
    # started, could never be nudged at all. Measured 2026-09-10: every agent
    # idle, each holding actionable work, and the loop reporting "no idle worker
    # with fresh open work". the operator: "all are stopped cant they have some other
    # tasks that has no dependency on blocked tasks."
    #
    # PENDING is exactly the set that is somebody's move to make (proposed,
    # assigned, active) and deliberately excludes blocked — so an agent stuck on
    # one thing is still offered the work that is NOT stuck, which is the whole
    # point. Union, not replacement: a board-only item stays nudgeable.
    owner_sessions = {}       # task id -> exact session named by the registry
    task_rooms = {}           # task id -> room id stamped by the registry
    task_states = {}          # task id -> registry state
    reset_room_cache()
    prio = {}                 # task id -> lead-set nudge rank
    try:
        import liljack_tasks as _reg
        for t in _reg.tasks(open_only=True):
            owner = (t.get("owner") or "").strip().lower()
            # ⚠ `held` is a deliberate park: it is OPEN but must never arm the
            # nudge. PENDING already excludes it, but state the rule here too so
            # a future edit cannot quietly re-arm a park.
            if t.get("state") == "held":
                continue
            owner_sessions[t["id"]] = (t.get("owner_session") or "").strip()
            task_rooms[t["id"]] = (t.get("room") or "").strip()
            task_states[t["id"]] = (t.get("state") or "").strip()
            try:
                prio[t["id"]] = int(t.get("priority") or 0)
            except (TypeError, ValueError):
                prio[t["id"]] = 0
            if not owner or t.get("state") not in _reg.PENDING:
                continue
            if t["id"] not in opens[owner]:
                opens[owner].append(t["id"])
        # ⚠ BOARD-ONLY IS NOT THE SAME AS BOARD-STALE, AND THE UNION ABOVE
        # COULD NOT TELL THEM APART. The union exists so an item that lives
        # ONLY on the board stays nudgeable. But a board row for a task the
        # REGISTRY has closed is not board-only work — it is a stale view, and
        # the registry is the queue. The gate loop below cannot save us: it
        # iterates `tasks(open_only=True)`, so a `done` task is never reached
        # and can never be removed from `opens` again.
        #
        # Measured 2026-09-10: room-phase-ui was `done` in the registry and
        # `active` on the board, and the heartbeat nudged its owner to finish
        # work that was already finished. Two more rows were in the same state.
        # The board lag itself is fixed at the source (liljack_tasks._save now
        # mirrors on every write); this is the independent guard, because a
        # view can always lag the store it mirrors.
        closed = {t["id"] for t in _reg.tasks(open_only=False)
                  if t.get("state") in ("done", "abandoned")}
        for owner in list(opens):
            opens[owner] = [t for t in opens[owner] if t not in closed]
    except Exception:
        pass                      # registry unreadable -> board-only, as before
    # ⚠ PRIORITY BEFORE AGE. A stable sort by descending rank keeps the board/
    # registry (state, age) order for equal priority — every existing task is 0,
    # so nothing changes until an assigner ranks work (liljack_tasks.set_priority).
    rank_tasks(opens, prio)
    stale, fresh = [], set()
    now_s = time.time()
    # ⚠ A DECLARED CADENCE MUST SCALE BOTH WINDOWS OR IT ONLY MOVES THE NOISE.
    # Widening freshness alone would keep trio-relaunch out of the owner nudge
    # and then let STALE_TASK_S (45m) mark it stale at 45 minutes and ESCALATE
    # it to the lead instead — the same interruption arriving by a different
    # door. A task that declares how often it is worth asking about is neither
    # nudged nor declared stalled inside that window.
    declared = {}
    try:
        import liljack_tasks as _cad
        for _t in _cad.tasks(open_only=True):
            try:
                _v = int(_t.get("checkin") or 0)
            except (TypeError, ValueError):
                _v = 0
            if _v > 0:
                declared[_t.get("id")] = _v
            # ⚠ THE REGISTRY OWNS "WHEN DID THIS LAST MOVE", NOT THE BOARD. The
            # board's timestamp is set by whoever last wrote the row, which is
            # the MIRROR — so any mirror bug re-dates every task and the whole
            # freshness calculation becomes fiction. That happened: a mirror
            # that rewrote unchanged rows stamped all ~112 of them with the same
            # instant, nothing was ever stale, and the heartbeat nudged nobody
            # for hours while agents sat idle. The mirror is fixed, but a VIEW's
            # timestamp should never have been the authority for a task the
            # STORE knows about. The board is still consulted for board-only
            # rows, which is the only case it is the sole record of.
    except Exception:
        pass                      # registry unreadable -> generic windows
    for t in review_panel.todos(since=scope):
        if t["state"] not in ("active", "queued"):
            continue
        try:
            age = now_s - datetime.fromisoformat(str(t["updated"]).replace("Z", "+00:00")).timestamp()
        except (ValueError, TypeError):
            continue
        # ⚠ THE UNION IS DELIBERATE — DO NOT MAKE THE REGISTRY AUTHORITATIVE
        # HERE. "Advanced recently" means advanced in EITHER record, because an
        # agent can mark the board directly without touching the registry, and
        # that is a real signal. I over-corrected once after the mirror bug and
        # test_liljack_worker_wake_fresh caught it ("a fresh BOARD row still
        # counts, registry notwithstanding"). The board going FALSELY fresh is a
        # mirror defect and is fixed where it belongs, in _mirror_rows: a mirror
        # makes the board agree with the registry, it never declares activity.
        window = declared.get(t["task"])
        if window:
            if age >= window:
                stale.append((t["agent"], t["task"]))
            else:
                fresh.add(t["task"])
            continue
        if age > STALE_TASK_S:
            stale.append((t["agent"], t["task"]))
        elif age < FRESH_TASK_S:
            fresh.add(t["task"])
    # ⚠ THE BOARD IS A VIEW; THE REGISTRY IS THE QUEUE. "Advanced recently" must
    # mean advanced in EITHER record (see the 2026-09-09 trio-relaunch incident).
    try:
        import liljack_tasks
        for t in liljack_tasks.tasks(open_only=True):
            if str(t.get("state") or "").lower() != "active":
                continue
            try:
                ts = datetime.fromisoformat(str(t.get("updated", "")).replace("Z", "+00:00"))
            except (ValueError, TypeError):
                continue
            # ⚠ ONE WINDOW DOES NOT FIT A WATCH TASK. FRESH_TASK_S is the right
            # rhythm for work somebody can advance on being asked; it is the
            # wrong one for a task whose whole job is to keep looking at
            # something for hours. trio-relaunch reported "healthy", aged out of
            # a 20-minute window, and was asked again — three times in an hour,
            # each costing a turn to answer identically. A task that knows its
            # own cadence declares it (liljack_tasks.set_checkin); absent means
            # the default, so nothing else changes.
            window = FRESH_TASK_S
            try:
                declared = int(t.get("checkin") or 0)
                if declared > 0:
                    window = declared
            except (TypeError, ValueError):
                pass              # a malformed value is not a licence to go quiet
            if now_s - ts.timestamp() < window:
                fresh.add(t.get("id"))
    except Exception:
        pass                      # registry unreadable -> board-only, as before

    # ⚠ AN ASSIGNMENT IS NOT PROGRESS, AND THE BOARD CANNOT TELL. sync_board
    # mirrors the registry, so a task CREATED minutes ago arrives on the board
    # freshly "updated" and lands in `fresh` — which silences the nudge for the
    # whole window on work nobody has touched. That is the assigned-vs-active
    # rule again, arriving through the board this time: only a task the REGISTRY
    # calls `active` has actually been picked up, and only that earns the quiet
    # window. Measured 2026-09-10: every agent idle, seven tasks "fresh", four
    # of them never started.
    try:
        import liljack_tasks as _reg
        started = {t["id"] for t in _reg.tasks(open_only=True) if t.get("state") == "active"}
        fresh = {t for t in fresh if t in started}
    except Exception:
        pass                      # registry unreadable -> keep the board's view

    # ⚠ A room phase or an open question blocks a todo from nudging: its owner
    # cannot act on it. Gate every registry task through codex's task_gate and
    # collect the blocked ones for the lead's digest — they must not vanish.
    # Unknown ("cannot tell") simply waits: removed from the nudge set, not
    # escalated (an unknown is not a blocked todo, it is a todo we cannot judge).
    gated = []
    try:
        import liljack_tasks as _tasks
        from liljack_app.room_protocol import task_gate as _task_gate
        for t in _tasks.tasks(open_only=True):
            owner = (t.get("owner") or "").strip().lower()
            if not owner:
                continue
            # ⚠ A HELD TASK IS NOT GATED WORK. It is deliberately parked by an
            # assigner, so it must not be surfaced as blocked either — the lead
            # put it down on purpose.
            if t.get("state") == "held":
                continue
            g = _task_gate(project, t, root=root)
            if g.get("allowed"):
                continue
            if t.get("id") in opens.get(owner, []):
                opens[owner].remove(t["id"])
            if g.get("unknown"):
                continue          # cannot tell -> wait, do not escalate
            gated.append({"owner": owner, "task": t["id"],
                          "reason": g.get("reason") or "blocked"})
    except Exception:
        pass                      # gate unavailable -> keep the old behaviour

    # ⚠ A BLOCKED REGISTRY TASK IS WORK THE ROOM IS STUCK ON. It must not arm the
    # spank — its owner cannot act on it — but it MUST reach the LEAD, or a queue
    # of blocked tasks rots while the lead is never told it exists. the operator's rule
    # ("spank everyone AND the leader"): the lead's digest names every blocked
    # task with its reason, so the lead can unblock, reassign or decide it.
    blocked = []
    try:
        import liljack_tasks as _tasks
        for t in _tasks.tasks(open_only=True):
            if t.get("state") != "blocked":
                continue
            owner = (t.get("owner") or "").strip().lower()
            note = (t.get("note") or "").strip()
            blocked.append({"owner": owner, "task": t["id"],
                            "reason": "blocked" + (f": {note[:120]}" if note else "")})
    except Exception:
        pass                      # registry unreadable -> surface nothing extra

    # ⚠ TMUX IS THE TRUTH. Discovery failure is "cannot tell", so the loop WAITS
    # rather than firing at a terminal it cannot confirm: `live` stays None and
    # we return before pick(), which would otherwise fall back to the DB row.
    live = None
    try:
        from liljack_app.runtime import Tmux
        t = tmux or Tmux()
        live = {r["id"] for r in t.sessions(project) if r.get("state") != "exited"}
    except Exception:
        return {"action": "worker-skip", "detail": "tmux liveness unknown"}

    # Mark dead terminals unavailable WITH the reason — escalate at once, never
    # retry. (A DB row that claims running but is absent from tmux is the exact
    # bug that made the heartbeat spank nobody.)
    for w in candidates(project, root):
        if w.get("role") == "lead":
            continue
        if w["id"] not in live and opens.get(w["agent"]) and not unavailable(w["id"]):
            mark_unavailable(w["id"], "no live terminal")
        elif w["id"] in live:
            # ⚠ UNAVAILABILITY IS A FACT ABOUT A TERMINAL, AND TERMINALS COME
            # BACK. The mark was one-way: set the moment tmux discovery missed a
            # session, and never cleared when it reappeared — while pick() skips
            # an unavailable agent PERMANENTLY ("never retried"). So one blip in
            # discovery removed an agent from the heartbeat for good.
            #
            # Measured 2026-09-10: deepseek carried "no live terminal" from
            # 11.5 hours earlier while his pane was live and idle with an open
            # task, and every tick reported "no idle worker with fresh open
            # work". the operator saw a dead heartbeat, and it was dead FOR HIM.
            #
            # ⚠ A FATAL reason is NOT cleared by a live terminal: no credit, an
            # API refusal or a crash loop are not fixed by the pane existing,
            # and re-nudging into one is how a strike ladder burns itself out.
            u = unavailable(w["id"])
            if u and _is_recoverable(u.get("reason") or ""):
                clear_unavailable(w["id"])

    try:
        notice = blocked_post_tick(project, root, live, lead_lifecycle.lookup, dry_run=dry_run)
    except (OSError, sqlite3.Error, ValueError):
        return {"action": "worker-skip", "detail": "blocked-post state unavailable"}
    if notice:
        return notice

    choice = pick(project, lead_lifecycle.lookup, opens, root=root,
                  stale=stale, fresh=fresh, live=live, gated=gated, blocked=blocked,
                  owner_sessions=owner_sessions, task_rooms=task_rooms,
                  task_states=task_states)
    if not choice:
        return {"action": "worker-idle-none",
                "detail": "no idle worker with fresh open work"}
    if dry_run:
        if choice["kind"] == "escalate":
            return {"action": "worker-dry-run",
                    "detail": f"would escalate {len(choice['escalations'])} item(s) to the lead",
                    "escalations": choice["escalations"]}
        return {"action": "worker-dry-run",
                "detail": f"would nudge {choice['agent']} about {choice['task']}"}

    res = nudge(project, choice)
    # Record EVERY attempt — a failure backs off exactly like a success, so an
    # unreachable agent cannot be retried forever with no memory of it.
    key = choice.get("task") or "digest"
    record(choice["session"], key, reason=res.get("reason", ""), delivered=bool(res.get("submitted")))
    if choice.get("kind") == "escalate" and choice.get("signature"):
        remember_digest(choice["session"], choice["signature"])

    if not res.get("submitted"):
        if _is_fatal(res.get("reason", "")):
            mark_unavailable(choice["session"], res.get("reason", ""))
        return {"action": "worker-nudged", **res}

    if choice["role"] == "lead" and strikes(choice["session"], key) >= STRIKES_ESCALATE:
        _escalate_operator(choice["agent"], choice.get("escalations") or [], send_mail, root=root)
        return {"action": "escalated-to-operator", "agent": choice["agent"], **res}
    return {"action": "worker-nudged", **res}


def question_tick(project, workspace_root=None, dry_run=True, now=None) -> dict:
    """Strike unanswered room questions against the lead, on the nudge cadence.

    This is the timer path for questions: a todo parked on a question is NOT
    armed (task_gate blocks it) and does NOT retire the heartbeat, and here the
    question accumulates strikes against the LEAD — escalating to the operator at
    codex's question_strike (STRIKES_ESCALATE), reusing the ladder rather than a
    parallel one. Rate-limited to COOLDOWN_S so a 5s tick does not hammer the
    workspace store or the lead.

    Returns {open_questions: int, results: [...]}. Never raises; a question state
    it cannot read is reported, never guessed (0 would read as "no questions").
    """
    now = now or time.time()
    s = _state()
    if now - s.get("questions:last", 0) < COOLDOWN_S:
        return {"open_questions": int(s.get("questions:open", 0)), "results": []}
    s["questions:last"] = now
    _write_state(s)
    try:
        from liljack_app.room_protocol import question_digest, question_strike, reconcile
        from liljack_workspace import Workspace
        store = Workspace(workspace_root)
    except Exception as exc:
        return {"open_questions": 0, "results": [{"error": type(exc).__name__}]}
    try:
        if not dry_run:
            reconcile(store, project)
        rows = question_digest(store, project)
        results = []
        for q in rows:
            if dry_run:
                results.append({"question": q["id"], "task": q["task"],
                                "strikes": q["strikes"]})
                continue
            try:
                results.append(question_strike(store, project, q["id"],
                                               f"hb-{int(now // COOLDOWN_S)}",
                                               "lead has not answered"))
            except Exception as exc:
                results.append({"question": q["id"], "error": type(exc).__name__})
        s = _state()
        s["questions:open"] = len(rows)
        _write_state(s)
        return {"open_questions": len(rows), "results": results}
    finally:
        store.close()
