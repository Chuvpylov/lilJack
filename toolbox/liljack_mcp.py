#!/usr/bin/env python3
"""
liljack_mcp.py — minimal stdio MCP server (pure stdlib, newline-delimited JSON-RPC).

Tools: project_context · project_todo · search_chains · project_history
       send_message · read_messages   (the Claude ↔ Codex mailbox)
       board_read · board_update      (the shared attention board, BMD)
       task_list · task_propose · task_assign · task_update
                                      (the durable task registry, BMD)
Read order: local mirror (~/.claude/session_archive/mcp-projects) → Apollo REST → live files.

Register:  claude mcp add --scope user liljack -- python3 <this file>
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from liljack_todo import find_todo, parse_todo, status_line
import liljack_common
import liljack_projects
import liljack_chains
import liljack_mail
import liljack_board
import liljack_tasks

RING_HEADER = os.environ.get("LILJACK_ARCHIVE_AUTH_HEADER", "X-Ring-Token")  # auth header of the remote archive

MIRROR = liljack_projects.MIRROR
CHAINS = liljack_chains.CHAINS_DIR
PROTO  = "2024-11-05"

TOOLS = [
    {"name": "project_context",
     "description": "Project digest: what it is, TODO state, recent work, hot files.",
     "inputSchema": {"type": "object", "properties": {"project": {"type": "string"}}}},
    {"name": "project_todo",
     "description": "Digest of the project's TODO.md: status, scope, pending items by "
                    "milestone. Pass full=true only when the whole file is genuinely "
                    "needed — it can be tens of KB.",
     "inputSchema": {"type": "object", "properties": {"project": {"type": "string"},
                     "full": {"type": "boolean"}}}},
    {"name": "search_chains",
     "description": "Keyword-search past reasoning chains (human msg → steps → outcome).",
     "inputSchema": {"type": "object", "properties": {"query": {"type": "string"},
                     "project": {"type": "string"}, "limit": {"type": "integer"}},
                     "required": ["query"]}},
    {"name": "project_history",
     "description": "Recent archived sessions for the project.",
     "inputSchema": {"type": "object", "properties": {"project": {"type": "string"},
                     "limit": {"type": "integer"}}}},
    {"name": "send_message",
     "description": "Send a message to the other agent working this machine "
                    "(claude | codex | deepseek | operator, or 'all'). Use it to hand over a task, "
                    "answer a question, or report a blocker — it is the only direct "
                    "channel between the harnesses. The body is secret-scrubbed and "
                    "archived with the session, so write it as work product. A room ID in thread "
                    "posts to that room; to_session always keeps the message private.",
     "inputSchema": {"type": "object", "properties": {
                     "to": {"type": "string", "description": "claude | codex | deepseek | operator | all"},
                     "text": {"type": "string"},
                     "subject": {"type": "string"},
                     "project": {"type": "string"},
                     "thread": {"type": "string", "description":
                             "r-… routes to that exact room unless to_session is set; otherwise mailbox thread metadata"},
                     "to_session": {"type": "string", "description":
                             "exact session id (s-…) to reach one session, e.g. another lead of your own agent"},
                     "frm": {"type": "string", "description":
                             "only when the harness cannot be detected from the environment"}},
                     "required": ["to", "text"]}},
    {"name": "read_messages",
     "description": "Read messages addressed to you. Unread by default; reading marks "
                    "them read unless mark_read=false. Your own messages are never in "
                    "your inbox — pass sent=true to check whether yours were read.",
     "inputSchema": {"type": "object", "properties": {
                     "unread_only": {"type": "boolean"},
                     "mark_read": {"type": "boolean"},
                     "sent": {"type": "boolean"},
                     "limit": {"type": "integer"},
                     "project": {"type": "string"},
                     "thread": {"type": "string"},
                     "agent": {"type": "string", "description":
                               "only when the harness cannot be detected from the environment"}},
                     }},
    {"name": "board_read",
     "description": "Read the shared attention board: what every agent on this machine "
                    "is working on, what is queued, blocked or done, plus board-level "
                    "notes. This is standing context, not chatter — it is already in "
                    "your prompt as a digest; call this for the full board or after "
                    "someone changes it. digest=true returns the short injected form.",
     "inputSchema": {"type": "object", "properties": {
                     "digest": {"type": "boolean"},
                     "agent": {"type": "string", "description":
                               "whose items to put first; defaults to you"}}}},
    {"name": "board_update",
     "description": "Add or update ONE item on the shared board, in place. Pass task "
                    "plus any of state (active|queued|blocked|done), progress, note — "
                    "omitted fields keep their value, so marking done does not erase "
                    "progress. Pass topic+value instead for a board-level note (a held "
                    "queue, a run everyone waits on). Pass not_doing=true with a task to "
                    "record an explicit neg() — do that whenever an absence matters, "
                    "because the toolbox_root engine cannot read a missing claim as false.",
     "inputSchema": {"type": "object", "properties": {
                     "task": {"type": "string"},
                     "state": {"type": "string", "description": "active | queued | blocked | done"},
                     "progress": {"type": "string", "description": "e.g. 220/600"},
                     "note": {"type": "string"},
                     "topic": {"type": "string", "description": "board note key, instead of task"},
                     "value": {"type": "string", "description": "board note value"},
                     "reason": {"type": "string"},
                     "not_doing": {"type": "boolean"},
                     "agent": {"type": "string", "description":
                               "only when the harness cannot be detected from the environment"}}}},
    {"name": "task_list",
     "description": "The durable task registry: every task, its owner, its state "
                    "(proposed → assigned → active → done, plus blocked and abandoned), "
                    "progress and note. THIS is the work queue — not the mailbox. Reading "
                    "a message has never finished a task and cannot consume one. Filter by "
                    "owner or state; pass task to get one task with its full brief and its "
                    "transition history.",
     "inputSchema": {"type": "object", "properties": {
                     "task": {"type": "string", "description": "one task id, with its brief and history"},
                     "owner": {"type": "string"},
                     "state": {"type": "string", "description": "proposed|assigned|active|blocked|done|abandoned"},
                     "open_only": {"type": "boolean", "description": "hide done and abandoned"},
                     "limit": {"type": "integer"}}}},
    {"name": "task_propose",
     "description": "Create a task. Leave owner empty to propose it for whoever picks it "
                    "up; you may set owner only to yourself (a claim) unless you are an "
                    "assigner. Give it a brief — the verbatim instructions the agent will "
                    "receive — plus acceptance (how it will be judged) and avoid (what not "
                    "to touch). Those three are what the handoff envelope renders.",
     "inputSchema": {"type": "object", "properties": {
                     "title": {"type": "string"},
                     "brief": {"type": "string"},
                     "acceptance": {"type": "string"},
                     "avoid": {"type": "string", "description": "what this task must not touch"},
                     "owner": {"type": "string"},
                     "note": {"type": "string"},
                     "task_id": {"type": "string", "description": "explicit id; default is a slug of the title"},
                     "agent": {"type": "string", "description":
                               "only when the harness cannot be detected from the environment"}},
                     "required": ["title"]}},
    {"name": "task_assign",
     "description": "Set a task's owner. the operator decides who does what: only an assigner may "
                    "hand work to somebody else. Anyone may CLAIM — take a task that is "
                    "unassigned or already their own — and that is what this tool does when "
                    "you name yourself or leave owner empty. Reassigning another agent's "
                    "task is refused, with the reason.",
     "inputSchema": {"type": "object", "properties": {
                     "task": {"type": "string"},
                     "owner": {"type": "string", "description": "defaults to you — a claim"},
                     "note": {"type": "string"},
                     "agent": {"type": "string"}},
                     "required": ["task"]}},
    {"name": "task_update",
     "description": "Move YOUR task through its lifecycle. op = start (→active) · block "
                    "(→blocked, note required, say what blocks it) · complete (→done) · "
                    "abandon (→abandoned, note required) · progress (record e.g. 340/600) · "
                    "unassign (hand it back, writing an explicit neg() so the absence is a "
                    "claim and not an inference). You may only move a task you own; "
                    "completing or abandoning another agent's task is refused.",
     "inputSchema": {"type": "object", "properties": {
                     "task": {"type": "string"},
                     "op": {"type": "string", "description":
                            "start|block|complete|abandon|progress|unassign"},
                     "note": {"type": "string"},
                     "progress": {"type": "string"},
                     "agent": {"type": "string"}},
                     "required": ["task", "op"]}},
]

AGENT = ""          # set by --agent; otherwise detected per call


def _slug(project: str = "") -> str:
    """Resolve the active project for a cwd.

    ⚠ Matches whole path SEGMENTS, deepest first — never a substring of the
    path. `/home/<user>/.../demo` contains the username,
    which is also an archived project name and one character longer than
    "demo"; a longest-substring match therefore resolved EVERY project on
    this machine to the user's personal archive.
    """
    if project:
        return project
    cwd = Path(os.getcwd()).resolve()
    if not MIRROR.is_dir():
        return cwd.name
    by_lower = {p.name.lower(): p.name for p in MIRROR.iterdir() if p.is_dir()}

    # A curated project name beats any directory that merely shares a name.
    known = liljack_common.project_name_of(str(cwd))
    if known and known.lower() in by_lower:
        return by_lower[known.lower()]

    for part in reversed(cwd.parts):
        if part.lower() in by_lower:
            return by_lower[part.lower()]
    return cwd.name


def _remote(path_in_repo: str) -> str:
    """Best-effort Apollo REST fallback: GET /api/repos/mcp-projects/files/<path>."""
    try:
        import urllib.request
        from session_archive import _apollo_url, _apollo_ring_token
        url = _apollo_url()
        if not url:
            return ""
        req = urllib.request.Request(f"{url}/api/repos/mcp-projects/files/{path_in_repo}")
        tok = _apollo_ring_token()
        if tok:
            req.add_header(RING_HEADER, tok)
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.read().decode(errors="replace")
    except Exception:
        return ""


def t_project_context(project: str = "") -> str:
    s = _slug(project)
    f = MIRROR / s / "context.md"
    if f.exists():
        return f.read_text(errors="replace")
    return _remote(f"{s}/context.md") or liljack_projects.build_context_md(s, os.getcwd())


TODO_DIGEST_BUDGET = 4000    # bytes; a full TODO.md runs to tens of KB


SUBHEAD_RE = re.compile(r'^(#{3,6})\s+(.+?)\s*$')


def _pending_groups(tp) -> list:
    """[(heading, [item, …]), …] for pending items, grouped by the nearest
    heading of ANY depth.

    parse_todo() tracks only `##`, so a file that organises its work under
    `###` collapses into one flat list where the grouping carries no
    information. This walks the file itself and keeps the deepest heading in
    scope, without changing parse_todo's contract (liljack todo / the statusline
    share it).
    """
    from liljack_todo import ITEM_RE, MILESTONE_RE, item_id
    groups, heading = [], ""
    for line in Path(tp).read_text(errors="replace").splitlines():
        m = MILESTONE_RE.match(line) or SUBHEAD_RE.match(line)
        if m:
            heading = m.group(m.lastindex)
            continue
        im = ITEM_RE.match(line)
        if im and im.group(2) not in "xX":
            if not groups or groups[-1][0] != heading:
                groups.append((heading, []))
            groups[-1][1].append({"id": item_id(im.group(3)), "text": im.group(3)})
    return groups


def _todo_digest(tp, parsed: dict) -> str:
    """Status + scope + pending items by heading, held under a byte budget."""
    out = [status_line(parsed)]
    if parsed["scope"]:
        out.append(f"\nscope: {parsed['scope'][:300]}")
    used, truncated = sum(len(l) + 1 for l in out), 0
    for heading, pending in _pending_groups(tp):
        header, wrote_header = f"\n{heading or '(no heading)'}", False
        for it in pending:
            line = f"  ○ {it['id']} {it['text'][:120]}"
            cost = len(line) + 1 + (0 if wrote_header else len(header) + 1)
            if used + cost > TODO_DIGEST_BUDGET:
                truncated += 1
                continue
            if not wrote_header:
                out.append(header)
                wrote_header = True
            out.append(line)
            used += cost
    if truncated:
        out.append(f"\n… {truncated} more pending items not shown (budget)")
    out.append(f"\nfull file: {tp} — re-call with full=true if you need all of it")
    return "\n".join(out)


def t_project_todo(project: str = "", full: bool = False) -> str:
    s = _slug(project)
    tp = find_todo(os.getcwd()) or (MIRROR / s / "TODO.md")
    if not Path(tp).exists():
        return f"no TODO.md for {s} — the rule says every project carries one (liljack todo init)"
    parsed = parse_todo(tp)
    if full:
        return f"{status_line(parsed)}\n\n" + Path(tp).read_text(errors="replace")
    return _todo_digest(tp, parsed)


def t_search_chains(query: str, project: str = "", limit: int = 5) -> str:
    words = set(re.findall(r'\b\w{3,}\b', query.lower()))
    hits = []
    if project:
        dirs = [CHAINS / project]
    else:
        dirs = [d for d in CHAINS.iterdir() if d.is_dir()] if CHAINS.is_dir() else []
    for d in dirs:
        if not d.is_dir():
            continue
        for f in d.glob("*.chains.jsonl"):
            for line in f.read_text(errors="replace").splitlines():
                try:
                    r = json.loads(line)
                except Exception:
                    continue
                hay = (r.get("human", "") + " " + r.get("final", "")).lower()
                score = sum(1 for w in words if w in hay)
                if score >= max(2, len(words) // 3):
                    hits.append((score, r))
    hits.sort(key=lambda x: -x[0])
    if not hits:
        return "no matching chains"
    out = []
    for score, r in hits[:limit]:
        o = r.get("outcome", {})
        out.append(f"[{r.get('ts','')[:10]} {r.get('session_id','')[:8]} score={score}]\n"
                   f"  human: {r.get('human','')[:150]}\n"
                   f"  steps: {len(r.get('steps',[]))} · tests={o.get('tests','none')} "
                   f"· files={','.join(o.get('files_edited',[])[:3])}\n"
                   f"  final: {r.get('final','')[:150]}")
    return "\n".join(out)


def t_project_history(project: str = "", limit: int = 10) -> str:
    f = MIRROR / _slug(project) / "sessions.jsonl"
    text = f.read_text(errors="replace") if f.exists() \
        else _remote(f"{_slug(project)}/sessions.jsonl")
    if not text:
        return f"no session history for {_slug(project)}"
    lines = text.strip().splitlines()[-limit:]
    out = []
    for line in reversed(lines):
        try:
            m = json.loads(line)
            out.append(f"{m.get('archived_at','?')[:10]}  {m.get('session_id','?')[:8]}"
                       f"  msgs={m.get('messages', m.get('message_count', '?'))}")
        except Exception:
            pass
    return "\n".join(out) or "empty history"


def _me(explicit: str = "") -> str:
    bound = (AGENT or liljack_mail.detect_agent()).strip().lower()
    if not AGENT and bound == "unknown":
        bound = ""
    explicit = explicit.strip().lower()
    if bound and explicit and explicit != bound:
        raise ValueError(f"identity {explicit!r} conflicts with this harness ({bound!r})")
    who = bound or explicit
    if who not in liljack_mail.AGENTS:
        raise ValueError("cannot tell which harness this is — pass the agent name "
                         f"explicitly (one of {liljack_mail.AGENTS}) or set LILJACK_AGENT")
    return who


def _room_post(project: str, room: str, text: str, frm: str = "") -> str:
    """Append `text` to the room stream as the calling session (identity checked)."""
    _me(frm)
    from liljack_workspace import Workspace
    from liljack_app.cli import caller
    import uuid
    room_project = project or os.environ.get("LILJACK_WORKSPACE_PROJECT", "")
    if not room_project:
        raise ValueError("room posting requires a workspace project")
    store = Workspace(os.environ.get("LILJACK_WORKSPACE_ROOT") or None)
    try:
        actor = caller(store, room_project)
        row = store.post(room_project, actor, text, uuid.uuid4().hex, destination=room)
    finally:
        store.close()
    return f"sent to room {row['destination']} · seq {row['seq']} · id {row['id']} · {len(row['text'])} chars"


def _caller_room() -> str:
    """The room of this launched session in the workspace store, or ""."""
    sid = os.environ.get("LILJACK_WORKSPACE_SESSION", "").strip()
    if not sid or not os.environ.get("LILJACK_WORKSPACE_PROJECT", ""):
        return ""
    from liljack_workspace import Workspace
    store = Workspace(os.environ.get("LILJACK_WORKSPACE_ROOT") or None)
    try:
        row = store.db.execute("SELECT room_id FROM room_members WHERE session_id=?", (sid,)).fetchone()
    finally:
        store.close()
    return (row[0] or "") if row else ""


def t_send_message(to: str, text: str, subject: str = "", project: str = "",
                   thread: str = "", frm: str = "", to_session: str = "") -> str:
    # Explicit session DMs take precedence even when carrying room metadata.
    if thread.startswith("r-") and not to_session:
        return _room_post(project, thread, text, frm)
    # ⚠ A ROOM MEMBER'S BROADCAST IS A ROOM POST. to='all' used to write only a
    # mailbox row while replying "sent to all (room r-…)", so a lead's landings
    # never reached the room stream and the worker waiting on them stayed blocked
    # (room r-bcbc0454, 2026-09-17; the operator: "fix the coordination bugs"). A
    # session outside any room keeps the untagged mailbox broadcast.
    if (to or "").strip().lower() == liljack_mail.EVERYONE and not to_session and not thread:
        room = _caller_room()
        if room:
            return _room_post(project, room, text, frm)
    row = liljack_mail.send(text, to, _me(frm), subject, project or _slug(), thread,
                            to_session=to_session)
    where = f"session {row['to_session']}" if row.get("to_session") else (
        f"mailbox of room {row['room']}, not the room stream" if row.get("room") else "all rooms (untagged)")
    return f"sent to {row['to']} ({where}) · id {row['id']} · {len(row['text'])} chars"


def t_read_messages(unread_only: bool = True, mark_read: bool = True, sent: bool = False,
                    limit: int = 20, project: str = "", thread: str = "", agent: str = "") -> str:
    me = _me(agent)
    if sent:
        rows = liljack_mail.sent(me, limit, project=project, thread=thread)
        return liljack_mail.render(rows, f"{len(rows)} sent by {me}") or "nothing sent"
    rows = liljack_mail.inbox(me, unread_only, limit, project, thread)
    if not rows:
        return f"no {'unread ' if unread_only else ''}mail for {me}"
    out = liljack_mail.render(rows, f"{len(rows)} message(s) for {me}")
    if mark_read:
        liljack_mail.ack([r["id"] for r in rows], me)
    return out


def t_board_read(digest: bool = False, agent: str = "") -> str:
    """The board, for whoever is asking. `_me` is not required here — reading a
    shared board is not an act attributable to an agent, so an undetectable
    harness still gets to see it (its own items simply are not hoisted)."""
    try:
        me = _me(agent)
    except ValueError:
        me = ""
    if digest:
        return liljack_board.digest(me or None) or "the board is empty"
    b = liljack_board.read()
    if not b["exists"]:
        return f"no board yet at {b['path']} — board_update creates it"
    out = [f"board {b['path']} · updated {b['updated']}"]
    for ag in liljack_board.AGENTS:
        rows = [i for i in b["items"] if i["agent"] == ag]
        rows.sort(key=lambda i: liljack_board._STATE_ORDER.get(i["state"], 9))
        out.append(f"{ag}{' (you)' if ag == me else ''}:" if rows
                   else f"{ag}{' (you)' if ag == me else ''}: nothing on the board")
        for i in rows:
            line = f"  {liljack_board._MARK.get(i['state'], '·')} {i['task']} — {i['state']}"
            if i["progress"]:
                line += f" {i['progress']}"
            if i["note"]:
                line += f" · {i['note']}"
            if i["updated"]:
                line += f" · updated {i['updated']}"
            out.append(line)
    for n in b["notes"]:
        out.append(f"note {n['topic']} = {n['value']}" + (f" — {n['reason']}" if n["reason"] else ""))
    for n in b["negs"]:
        out.append("NOT " + "/".join(n["args"]) + (f" — {n['reason']}" if n["reason"] else ""))
    if b["stray"]:
        out.append(f"⚠ {b['stray']} assertion(s) live in a pasem footer and are not "
                   "managed by the board API")
    if b["errors"]:
        out.append("⚠ the board has at least one malformed claim — it was parsed leniently")
    return "\n".join(out)


def t_board_update(task: str = "", state: str = "", progress: str = "", note: str = "",
                   topic: str = "", value: str = "", reason: str = "",
                   not_doing: bool = False, agent: str = "") -> str:
    if topic:
        if not value:
            return "a board note needs both topic and value"
        row = liljack_board.set_note(topic, value, reason or None)
        return f"note {row['topic']} = {row['value']} · updated {row['updated']}"
    if not task:
        return "pass task (with state/progress/note) or topic+value"
    me = _me(agent)
    if not_doing:
        row = liljack_board.negate(me, task, reason)
        return f"recorded NOT {me}/{row['args'][1]} — {row['reason'] or 'no reason given'}"
    row = liljack_board.upsert(me, task, state or None,
                               progress if progress else None,
                               note if note else None)
    return (f"{row['agent']}/{row['task']} → {row['state']}"
            + (f" {row['progress']}" if row["progress"] else "")
            + f" · updated {row['updated']}")



# ── the task registry ────────────────────────────────────────────────────────
# ⚠ A refusal is an ANSWER, not an error. `Denied` carries the rule that was
# broken ("codex may not assign work to claude — only ('operator', 'claude')
# assign"), and an agent that gets that text learns the boundary; a JSON-RPC
# -32603 teaches it nothing and reads as a broken server.

def _task_error(exc) -> str:
    return f"refused: {exc}"


def _mirror():
    """Kept as a no-op call site: the mirror moved INTO the registry write.

    ⚠ This function used to be the ONLY thing that mirrored the registry onto
    the attention board, and that was the defect. A write through MCP updated
    the board; a write through the library or the CLI did not, and the two
    stores then disagreed silently and forever. Measured 2026-09-10: three
    tasks `done` in the registry and still `active` on the board, so the
    heartbeat nudged their owners about finished work.

    `liljack_tasks._save()` — the single write funnel — now mirrors on every
    write, so the board follows the registry no matter who writes. Nothing is
    called here on purpose: a second copy of the rule is a second opinion, and
    that is what this layer exists to end.
    """
    return None


def _fmt_task(t: dict) -> str:
    line = (f"{liljack_tasks._STATE_MARK.get(t['state'], '?')} {t['id']} — {t['state']}"
            f" · owner {t['owner'] or '(unassigned)'}")
    if t.get("progress"):
        line += f" · {t['progress']}"
    if t.get("title"):
        line += f" · {t['title']}"
    if t.get("note"):
        line += f"\n    note: {t['note']}"
    return line


def t_task_list(task: str = "", owner: str = "", state: str = "", open_only: bool = False,
                limit: int = 50) -> str:
    try:
        if task:
            t = liljack_tasks.get(task)
            out = [liljack_tasks.envelope(t),
                   f"\nstate {t['state']} · owner {t['owner'] or '(unassigned)'} "
                   f"· by {t['by']} · created {t['created']} · updated {t['updated']}"]
            if t["progress"]:
                out.append(f"progress: {t['progress']}")
            if t["note"]:
                out.append(f"note: {t['note']}")
            out.append("history:")
            for r in liljack_tasks.history(task):
                out.append(f"  {r['at']}  {r['op']:<9} {r['state']:<10} by {r['by']}"
                           + (f"  {r['note']}" if r["note"] else ""))
            return "\n".join(out)
        rows = liljack_tasks.tasks(owner, state, open_only)
    except (liljack_tasks.Denied, liljack_tasks.TaskError) as exc:
        return _task_error(exc)
    # ⚠ A LAUNCHED SESSION DOES NOT SEE ITS AGENT'S OTHER SESSIONS' ROWS AS ITS
    # OWN (coord-c, room r-bcbc0454): hide them and say how many were hidden.
    mine = os.environ.get("LILJACK_WORKSPACE_SESSION", "").strip()
    hidden = 0
    if mine:
        try:
            me = _me("")
        except ValueError:
            me = ""
        if me:
            keep = [t for t in rows if not (t["owner"] == me and (t.get("owner_session") or "").strip()
                                            and t["owner_session"].strip() != mine)]
            hidden = len(rows) - len(keep)
            rows = keep
    rows = rows[:max(1, int(limit))]
    tail = (f"\n({hidden} {me} task{'s' if hidden != 1 else ''} bound to other sessions hidden"
            " — task_list task=<id> still shows one)") if hidden else ""
    if not rows:
        return "no tasks match — the registry is the queue, so an empty list means no work" + tail
    return "\n".join(_fmt_task(t) for t in rows) + tail


def t_task_propose(title: str, brief: str = "", acceptance: str = "", avoid: str = "",
                   owner: str = "", note: str = "", task_id: str = "", agent: str = "") -> str:
    # ⚠ A TASK PROPOSED FROM INSIDE A ROOM IS STAMPED WITH THAT ROOM AND THE
    # EXACT OWNER SESSION (fix-cross-room-session-split). Bare propose stamped
    # neither, and the heartbeat then routed such rows by agent slot into any
    # room. Owner == me -> my session; owner == another agent -> that agent's
    # single member in my room (ambiguous -> left blank, room still stamped).
    try:
        me = _me(agent)
        me_session = os.environ.get("LILJACK_WORKSPACE_SESSION", "").strip()
        room = liljack_mail.session_room(me_session) if me_session else ""
        owner_session = ""
        if owner and room:
            if owner == me:
                owner_session = me_session
            else:
                hits = [sid for sid, ag in liljack_mail.room_sessions(room) if ag == owner]
                owner_session = hits[0] if len(hits) == 1 else ""
        row = liljack_tasks.propose(title, me, owner, brief, acceptance,
                                    avoid, note, task_id=task_id, room=room,
                                    owner_session=owner_session)
    except (liljack_tasks.Denied, liljack_tasks.TaskError, ValueError) as exc:
        return _task_error(exc)
    _mirror()
    return (f"{row['id']} → {row['state']}"
            + (f", owner {row['owner']}" if row["owner"] else ", unassigned")
            + f" · {row['title']}")


def t_task_assign(task: str, owner: str = "", note: str = "", agent: str = "") -> str:
    try:
        me = _me(agent)
        row = liljack_tasks.assign(task, owner or me, me, note)
    except (liljack_tasks.Denied, liljack_tasks.TaskError, ValueError) as exc:
        return _task_error(exc)
    _mirror()
    return f"{row['id']} → {row['state']}, owner {row['owner']} · updated {row['updated']}"


_OPS = {"start": "active", "block": "blocked", "complete": "done", "abandon": "abandoned"}


def t_task_update(task: str, op: str, note: str = "", progress: str = "",
                  agent: str = "") -> str:
    op = (op or "").strip().lower()
    try:
        me = _me(agent)
        if op in _OPS:
            row = liljack_tasks.set_state(task, _OPS[op], me, note,
                                          progress or None)
        elif op == "progress":
            if not progress:
                return "progress needs a value, e.g. 340/600"
            row = liljack_tasks.set_progress(task, progress, me, note or None)
        elif op == "unassign":
            row = liljack_tasks.unassign(task, me, note)
        else:
            return f"unknown op {op!r} — one of start, block, complete, abandon, progress, unassign"
    except (liljack_tasks.Denied, liljack_tasks.TaskError, ValueError) as exc:
        return _task_error(exc)
    _mirror()
    return (f"{row['id']} → {row['state']}"
            + (f" {row['progress']}" if row["progress"] else "")
            + f" · owner {row['owner'] or '(unassigned)'} · updated {row['updated']}")


HANDLERS = {"project_context": t_project_context, "project_todo": t_project_todo,
            "search_chains": t_search_chains, "project_history": t_project_history,
            "send_message": t_send_message, "read_messages": t_read_messages,
            "board_read": t_board_read, "board_update": t_board_update,
            "task_list": t_task_list, "task_propose": t_task_propose,
            "task_assign": t_task_assign, "task_update": t_task_update}


def handle(req: dict):
    method, params = req.get("method", ""), req.get("params") or {}
    if method == "initialize":
        return {"protocolVersion": PROTO, "capabilities": {"tools": {}},
                "serverInfo": {"name": "liljack", "version": "1.0.0"}}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        fn = HANDLERS.get(params.get("name", ""))
        if not fn:
            raise LookupError(f"unknown tool {params.get('name')!r}")
        try:
            text = fn(**(params.get("arguments") or {}))
        except TypeError as e:
            text = f"bad arguments: {e}"
        return {"content": [{"type": "text", "text": text}]}
    raise LookupError(f"method not found: {method}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mirror", default="")   # test override
    ap.add_argument("--chains", default="")   # test override
    ap.add_argument("--agent", default="", help="claude|codex|operator — who this server "
                    "speaks for, when the environment cannot say")
    args = ap.parse_args()
    global MIRROR, CHAINS, AGENT
    AGENT = args.agent.strip().lower()
    if args.mirror:
        MIRROR = Path(args.mirror)
        liljack_projects.MIRROR = MIRROR
    if args.chains:
        CHAINS = Path(args.chains)
        liljack_chains.CHAINS_DIR = CHAINS
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            continue
        if "id" not in req:            # notification — no response
            continue
        resp = {"jsonrpc": "2.0", "id": req["id"]}
        try:
            resp["result"] = handle(req)
        except LookupError as e:
            resp["error"] = {"code": -32601, "message": str(e)}
        except Exception as e:
            resp["error"] = {"code": -32603, "message": str(e)}
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
