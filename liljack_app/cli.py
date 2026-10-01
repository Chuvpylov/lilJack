"""Room and session commands for the native lilJack frontend.

No GUI/rendering dependencies are imported. Launch environment identities are
checks for cooperating same-user clients, not an authentication sandbox: a
process running as the same Unix user can change its environment/database.
Mailbox addresses providers; session DMs stay private in the workspace and are
never exported to provider mailboxes. Named rooms are also workspace-only. Reads never acknowledge
mail and room posts never inject terminal input.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import uuid

from liljack_mail import AGENTS, detect_agent, _scrub
from liljack_workspace import Workspace, clean, project_key, ROOM_STATES
from liljack_room_bridge import sync_workspace
from .runtime import Tmux


def caller(store, project):
    """Resolve a launched session, failing closed on absent/mismatched identity.

    Explicit LILJACK_AGENT=operator without a session is the operator CLI path.
    A shell session declares provider operator but retains its individual session
    identity and therefore its assigned control scope.
    """
    declared = os.environ.get("LILJACK_AGENT", "").strip().lower()
    if declared and declared not in AGENTS:
        raise PermissionError("Unknown declared agent identity")
    provider = detect_agent()
    sid = os.environ.get("LILJACK_WORKSPACE_SESSION", "").strip()
    if sid:
        row = store.db.execute("SELECT project,data FROM sessions WHERE id=?", (sid,)).fetchone()
        if not row or row["project"] != project_key(project):
            raise PermissionError("Workspace session is missing or belongs to another project")
        session = json.loads(row["data"])
        expected = "operator" if session.get("agent") == "shell" else session.get("agent")
        if provider not in AGENTS or expected != provider or session.get("harness") == "mailbox":
            raise PermissionError("Workspace session does not match the launched agent")
        return sid
    if declared == "operator" and provider == "operator":
        return "operator"
    raise PermissionError("No workspace session identity; launch this agent through lilJack")


def _sync_room(store, project):
    try:
        return sync_workspace(store, project)
    except (OSError, ValueError) as exc:
        # Counts are only known for a completed sync. Local room operations
        # still work; the incomplete marker prevents claiming delivery.
        return {"imported": 0, "exported": 0, "incomplete": True,
                "issues": [{"reason": "Mailbox synchronization unavailable", "public": True}]}


def room_cli(argv):
    p = argparse.ArgumentParser(description="Read/post shared room; private messages use exact session IDs")
    p.add_argument("--root")
    p.add_argument("--project", default=os.environ.get('LILJACK_WORKSPACE_PROJECT') or os.getcwd())
    operation = p.add_mutually_exclusive_group()
    operation.add_argument("--text")
    # Message bodies typed as shell arguments are the root cause of the 2026-09-12
    # incident: backticks and $( ) inside a double-quoted --text were substituted
    # by the shell (a `git stash` ran in the demo tree). The fix is the
    # transport, not after-the-fact detection: read the body from a file or stdin.
    operation.add_argument("--text-file", metavar="PATH",
                           help="post the file's exact bytes as --text")
    operation.add_argument("--message", help="read one visible message and acknowledge its delivery")
    # Shared room canvas (rooms/<room-id>/canvas.jsonl); lilJack renders it sixel-first.
    operation.add_argument('--draw', metavar='JSON', help='append one canvas stroke to --to room (see CANVAS-FORMAT.md)')
    operation.add_argument('--draw-file', metavar='PATH', help='append every JSON line of PATH as canvas strokes')
    operation.add_argument('--canvas-clear', action='store_true', help='append a clear stroke to the room canvas')
    operation.add_argument('--draw-image', metavar='PATH', help='copy an image into the room media folder and place it on the canvas (--at x0,y0,x1,y1; default centred, aspect kept)')
    operation.add_argument('--attach', metavar='PATH', help="copy a file into the room media folder and post a 'media: PATH' line to --to room")
    operation.add_argument('--canvas-list', action='store_true', help='list canvas strokes: id, kind, author, bbox, text/file')
    operation.add_argument('--erase', metavar='ID[,ID]', help='delete canvas strokes by id')
    operation.add_argument('--canvas-move', dest='move_ids', metavar='ID[,ID]', help='move canvas strokes by id (with --by-delta dx,dy)')
    operation.add_argument('--undo', action='store_true', help="delete the caller's most recent canvas stroke")
    operation.add_argument('--set-view', metavar='WHO=FMT', help='choose how a member sees the room canvas: png, jpeg, svg, pdf, live (join with +)')
    operation.add_argument('--canvas-view', action='store_true', help="export the room canvas in the caller's chosen formats (see --set-view) and print the paths")
    operation.add_argument('--canvas-png', metavar='OUT', help='render the room canvas (lines and images) to a PNG you can open (--size WxH, default 1600x900)')
    operation.add_argument("--create", metavar="NAME")
    operation.add_argument('--start', metavar='JSON_FILE', help='start a room from the room_start JSON contract')
    operation.add_argument('--context', action='store_true', help='read current room context and acknowledge intro')
    operation.add_argument('--align', metavar='UNDERSTANDING', help='acknowledge ALIGN in one line, then wait')
    operation.add_argument('--phase', choices=('ALIGN','PLAN','WORK','REVIEW','RESOLVED'))
    operation.add_argument('--plan', metavar='JSON_FILE', help='lead writes room todos with exact owner_session IDs')
    operation.add_argument('--question', metavar='TEXT', help='park --task with a question for the lead')
    operation.add_argument('--answer', metavar='QUESTION_ID', help='lead answers --answer-text and unblocks work')
    p.add_argument('--task', help='exact room todo for --question')
    p.add_argument('--answer-text')
    p.add_argument('--folder', help='working directory for --create')
    p.add_argument('--purpose', default='', help='one-line room purpose for --create')
    operation.add_argument("--collapse", metavar="ROOM")
    operation.add_argument("--expand", metavar="ROOM")
    operation.add_argument("--resolve", metavar="ROOM", help="mark a room finished; keeps all history")
    operation.add_argument("--reopen", metavar="ROOM", help="undo --resolve")
    operation.add_argument("--move", metavar="SESSION")
    operation.add_argument('--state', choices=ROOM_STATES, help='set --to room lifecycle state')
    operation.add_argument('--purge', metavar='ROOM', help='purge catalog after 30 days in trash; retain history')
    operation.add_argument('--move-all', metavar='ROOM', help='atomically move roster into --into room')
    p.add_argument('--force', action='store_true', help='explicitly archive/remove despite live sessions; never stops them')
    p.add_argument("--into", default="", metavar="ROOM", help="room for --move; omitted means standalone")
    p.add_argument("--to", help="destination; defaults to the session's current room, otherwise the public lobby")
    p.add_argument('--at', metavar='X0,Y0,X1,Y1', help='with --draw-image: normalized rect on the 16:9 picture')
    p.add_argument('--by', dest='only', metavar='AGENT', help="with --canvas-view: only that agent's layer")
    p.add_argument('--by-delta', metavar='DX,DY', help='with --canvas-move: normalized offset, each -1..1')
    p.add_argument('--size', metavar='WxH', default='1600x900', help='with --canvas-png')
    p.add_argument("--reply")
    p.add_argument("--language", default="und")
    p.add_argument("--request-id")
    p.add_argument("--since", type=int)
    p.add_argument("--messages-only", action="store_true", help="omit lifecycle events from --since reads")
    a = p.parse_args(argv)
    store = Workspace(a.root)
    try:
        actor = caller(store, a.project)
        if a.draw is not None or a.draw_file is not None or a.canvas_clear or a.draw_image is not None or a.canvas_png is not None \
                or a.set_view is not None or a.canvas_view or a.canvas_list or a.erase is not None \
                or a.move_ids is not None or a.undo:
            from . import canvas
            membership = store.db.execute('SELECT room_id FROM room_members WHERE session_id=?', (actor,)).fetchone()
            room = a.to or (membership[0] if membership else '')
            author = actor if actor == 'operator' else detect_agent()
            if a.canvas_list:
                print(json.dumps({'strokes': canvas.listing(store.root, room)}, ensure_ascii=False))
                return 0
            if a.undo:
                print(json.dumps(canvas.undo(store.root, room, author), ensure_ascii=False))
                return 0
            if a.erase is not None or a.move_ids is not None:
                ids = [i.strip() for i in (a.erase if a.erase is not None else a.move_ids).split(',') if i.strip()]
                op = {'t': 'del', 'ids': ids}
                if a.move_ids is not None:
                    parts = (a.by_delta or '').split(',')
                    if len(parts) != 2:
                        raise ValueError('--canvas-move needs --by-delta dx,dy')
                    op = {'t': 'move', 'ids': ids, 'd': [float(v) for v in parts]}
                print(json.dumps(canvas.append(store.root, room, [op], author), ensure_ascii=False))
                return 0
            if a.set_view is not None:
                print(json.dumps(canvas.set_view(store.root, room, a.set_view), ensure_ascii=False))
                return 0
            if a.canvas_view:
                session = actor if actor.startswith('s-') else None
                print(json.dumps(canvas.view(store.root, room, author, session, a.only), ensure_ascii=False))
                return 0
            if a.canvas_png is not None:
                width, height = canvas.parse_size(a.size)
                print(json.dumps(canvas.png(store.root, room, Path(a.canvas_png).resolve(), width, height), ensure_ascii=False))
                return 0
            if a.draw_image is not None:
                rect = tuple(float(v) for v in a.at.split(',')) if a.at else None
                if rect is not None and len(rect) != 4:
                    raise ValueError('--at is x0,y0,x1,y1')
                print(json.dumps(canvas.append_image(store.root, room, a.draw_image, author, rect), ensure_ascii=False))
                return 0
            if a.canvas_clear:
                strokes = [{'t': 'clear'}]
            elif a.draw is not None:
                strokes = [json.loads(a.draw)]
            else:
                strokes = [json.loads(line) for line in Path(a.draw_file).read_text(encoding='utf-8').splitlines() if line.strip()]
            print(json.dumps(canvas.append(store.root, room, strokes, author), ensure_ascii=False))
            return 0
        if a.align is not None or a.phase or a.plan or a.question is not None or a.answer:
            from . import room_protocol as protocol
            membership = store.db.execute('SELECT room_id FROM room_members WHERE session_id=?', (actor,)).fetchone()
            room = a.to or (membership[0] if membership else '')
            if a.align is not None:
                value = protocol.align(store, a.project, room, actor, a.align)
            elif a.phase:
                value = protocol.transition(store, a.project, room, a.phase, actor)
            elif a.plan:
                value = protocol.plan(store, a.project, room, actor, json.loads(Path(a.plan).read_text()), a.request_id or uuid.uuid4().hex)
            elif a.question is not None:
                value = protocol.ask(store, a.project, room, a.task, actor, a.question, a.request_id or uuid.uuid4().hex)
            else:
                value = protocol.answer(store, a.project, a.answer, actor, a.answer_text)
            print(json.dumps(value, ensure_ascii=False))
            return 0
        if a.state is not None or a.purge is not None or a.move_all is not None:
            if actor != 'operator':
                raise PermissionError('only the operator can change room lifecycle or move teams')
            if a.state is not None or a.purge is not None:
                store.refresh(a.project)
                for row in Tmux().sessions(a.project):
                    store.observe(row)
            if a.state is not None:
                result = {'changed_room': store.room_state(a.project, a.to or '', a.state, actor=actor, force=a.force)}
                from .room_instructions import reconcile
                reconcile(store, a.project, a.to or '')
            elif a.purge is not None:
                result = store.room_purge(a.project, a.purge, actor=actor)
            else:
                result = store.room_move_all(a.project, a.move_all, a.into, actor=actor)
            print(json.dumps({**store.snapshot(a.project, viewer=actor), **result}, ensure_ascii=False))
            return 0
        # Startup/context must not bridge provider mail or acknowledge unrelated notices.
        if a.start is not None:
            from .room_start import start_room
            req = json.loads(Path(a.start).read_text())
            if not isinstance(req, dict):
                raise ValueError('room_start JSON must be an object')
            if a.request_id:
                req['request_id'] = a.request_id
            result = start_room(store, Tmux(), a.project, req, actor=actor)
            print(json.dumps({**store.snapshot(a.project, viewer=actor), **result}, ensure_ascii=False))
            return 0
        if a.context:
            from .room_start import room_context
            membership = store.db.execute('SELECT room_id FROM room_members WHERE session_id=?', (actor,)).fetchone()
            room_id = a.to or (membership[0] if membership else '')
            result = room_context(store, a.project, room_id, actor, acknowledge=True)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        if a.attach is not None:
            # A screenshot or file for the room: the copy lives beside the canvas,
            # the message is an ordinary post whose line lilJack can open (media tile).
            from . import canvas
            membership = store.db.execute('SELECT room_id FROM room_members WHERE session_id=?', (actor,)).fetchone()
            copy = canvas.import_file(store.root, a.to or (membership[0] if membership else ''), a.attach)
            a.text = 'media: %s' % copy
        before = _sync_room(store, a.project)
        if a.message is not None:
            from .room_delivery import read_message
            result = read_message(store, a.project, actor, a.message)
            bridge = before
        elif a.create is not None:
            room = store.room_create(a.project, a.create, actor=actor, folder=a.folder, purpose=a.purpose)
            result = {**store.snapshot(a.project, viewer=actor), "created_room": room["id"]}
            bridge = before
        elif a.collapse is not None or a.expand is not None:
            store.room_update(a.project, a.collapse or a.expand, a.collapse is not None, actor=actor)
            result, bridge = store.snapshot(a.project, viewer=actor), before
        elif a.resolve is not None or a.reopen is not None:
            # Resolving is a label, never a delete: the room's messages, members
            # and delivery history are the record of how the work was done.
            store.room_resolve(a.project, a.resolve or a.reopen, a.resolve is not None, actor=actor)
            # the folder's fragment follows whichever room is still live there
            from .room_instructions import reconcile
            reconcile(store, a.project, a.resolve or a.reopen)
            result, bridge = store.snapshot(a.project, viewer=actor), before
        elif a.move is not None:
            store.room_move(a.project, a.move, a.into, actor=actor)
            result, bridge = store.snapshot(a.project, viewer=actor), before
        elif a.text is not None or a.text_file is not None:
            # --text-file PATH and --text - (stdin) carry the body verbatim: the
            # shell never sees it, so backticks and $( ) stay literal.
            if a.text_file is not None:
                body = Path(a.text_file).read_text(encoding='utf-8')
            elif a.text == '-':
                body = sys.stdin.read()
            else:
                body = a.text
            membership = store.db.execute("SELECT room_id FROM room_members WHERE session_id=?", (actor,)).fetchone()
            destination = a.to if a.to is not None else (membership[0] if membership else "room")
            result = store.post(a.project, actor, body, a.request_id or uuid.uuid4().hex,
                                destination=destination, language=a.language, reply_to=a.reply)
            after = _sync_room(store, a.project)
            bridge = {"imported": before["imported"] + after["imported"],
                      "exported": before["exported"] + after["exported"],
                      "issues": before["issues"] + after["issues"],
                      "incomplete": bool(before.get("incomplete") or after.get("incomplete"))}
        elif a.since is not None:
            result = store.events(a.project, a.since, viewer=actor, messages_only=a.messages_only)
            bridge = before
        else:
            result = store.snapshot(a.project, viewer=actor)
            bridge = before
        # Delivery failures are explicit, without disclosing unrelated private IDs.
        visible = {m["id"] for m in (store._message(row) for row in store.db.execute(
            "SELECT * FROM messages WHERE project=?", (project_key(a.project),)))
            if store._visible(m, actor)}
        bridge["issues"] = [issue for issue in bridge["issues"] if actor == "operator" or
                             issue.get("public") or issue.get("message_id") in visible]
        print(json.dumps({**result, "bridge": bridge}, ensure_ascii=False))
    finally:
        store.close()
    return 0


def control_cli(argv):
    p = argparse.ArgumentParser(description="Control assigned descendants; explicit operator controls all sessions")
    p.add_argument("action", choices=["list", "send", "submit", "interrupt", "stop", "assign", "spawn", "compact"])
    p.add_argument("--root")
    p.add_argument("--project", default=os.environ.get('LILJACK_WORKSPACE_PROJECT') or os.getcwd())
    p.add_argument("--session")
    p.add_argument("--text")
    p.add_argument("--role", choices=["lead", "worker", "reviewer"], default="worker")
    p.add_argument("--parent")
    p.add_argument('--room', help='spawn in this room with its folder and intro')
    p.add_argument("--agent", choices=["claude", "codex", "deepseek", "shell"], default="shell")
    p.add_argument("--backend", choices=["tmux", "native"], default="tmux",
                   help="native talks to liljack-sessiond (no tmux); default tmux until a clean working day")
    a = p.parse_args(argv)
    store = Workspace(a.root)
    try:
        actor = caller(store, a.project)
        sessions = store.snapshot(a.project, viewer=actor)["sessions"]
        if a.action == "list":
            print(json.dumps([{**s, "controllable": store.may_control(actor, s["id"])} for s in sessions], ensure_ascii=False))
            return 0
        if a.action == "compact":
            # Handoff digest: a session may compact its own record; the operator
            # may compact any. Read-only on the archive/index — see liljack_handoff.
            if actor != "operator" and actor != a.session:
                raise PermissionError("compact your own session, or ask the operator")
            import liljack_handoff
            result = liljack_handoff.compact(store, a.project, a.session)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        if a.action == "assign":
            if not any(s["id"] == a.session for s in sessions):
                raise ValueError("Target session is not in this project")
            result = store.assign(a.session, a.role, a.parent, actor=actor)
        elif a.action == "spawn":
            if a.room:
                from .room_start import create_session
                if a.parent:
                    expected = store.spawn_parent(a.project, a.room, a.role, actor=actor)
                    if a.parent != expected:
                        raise ValueError('parent must be the room lead')
                sid = create_session(store, Tmux(), a.project, a.agent, actor=actor,
                                     room_id=a.room, role=a.role, backend=a.backend)
                print(json.dumps({'session_id': sid, 'room_id': a.room, 'role': a.role,
                                  'backend': a.backend}))
                return 0
            parent = store.authorize_spawn(a.project, actor=actor, parent_id=a.parent, role=a.role)
            tmux = Tmux()
            sid = tmux.create(a.agent, a.project, store.root, backend=a.backend)
            row = next((r for r in tmux.sessions(a.project) if r["id"] == sid), None)
            if not row:
                raise RuntimeError("Created terminal was not discoverable; inspect lilJack tmux before retrying")
            store.observe(row)
            store.assign_spawned(sid, a.role, parent, actor=actor)
            if row.get("backend") == "native":
                from .runtime import NativeBackend
                store.bind_terminal(sid, NativeBackend().sock, row["tmux_name"],
                                    row.get("daemon_id") or row["tmux_name"], actor=actor)
            else:
                store.bind_terminal(sid, tmux.socket, row["tmux_name"], row["pane"], actor=actor)
            result = {"session_id": sid, "parent": parent, "role": a.role, "backend": a.backend}
        else:
            # Two different failures used to share one message, and that cost a real
            # misdiagnosis: a truncated id reads as an ACL denial. Say which it is, and
            # name the caller's role so a mid-sequence demotion is legible rather than
            # looking like a broken permission model.
            if not any(s["id"] == a.session for s in sessions):
                raise ValueError(f"No session {a.session} in this project - pass the full "
                                 "32-character id, not a display truncation")
            if not store.may_control(actor, a.session):
                mine = next((s for s in sessions if s["id"] == actor), None)
                role = mine["role"] if mine else "not a participant in this project"
                raise PermissionError(f"Target is outside your session control scope - you are "
                                      f"{role}; only a lead controls its own descendants")
            text = None
            if a.action == "send":
                text = clean(a.text)
                if any(ord(c) < 32 or 127 <= ord(c) < 160 for c in text):
                    raise ValueError("Send stages one line; control characters are not allowed")
            tmux = Tmux()
            live = next((s for s in tmux.sessions(a.project) if s["id"] == a.session), None)
            if not live or live["state"] == "exited":
                raise ValueError("Target has no live terminal")
            result = {"session_id": a.session, "action": a.action, "status": "sent_to_tmux"}
            if a.action == "send":
                tmux.call("send-keys", "-t", live["pane"], "-l", "--", text)
            elif a.action == "submit":
                # ⚠ SUBMIT MUST VERIFY, NOT ASSERT. "sent_to_tmux" proves only that
                # tmux accepted the Enter — not that a turn started (the operator 2026-09-09:
                # a pasted instruction sat unsubmitted under the screensaver). Poll the
                # target's lifecycle for a NEW event and report delivered / queued /
                # unverified, so the caller can tell a real wake from a held line.
                from . import lead_lifecycle
                harness = {"deepseek": "opencode", "codex": "codex", "claude": "claude"}.get(live["agent"])
                before = lead_lifecycle.lookup(a.session, a.project, harness,
                                               workspace_root=a.root) if harness else None
                tmux.call("send-keys", "-t", live["pane"], "Enter")
                if harness:
                    outcome, detail = lead_lifecycle.verify_submit(
                        a.session, a.project, harness, before, workspace_root=a.root)
                    result.update(status=outcome, detail=detail)
            elif a.action == "interrupt":
                tmux.call("send-keys", "-t", live["pane"], "C-c")
            elif a.action == "stop":
                tmux.call("kill-session", "-t", live["tmux_name"])
                store.unbind_terminal(a.session, actor=actor)
        print(json.dumps(result, ensure_ascii=False))
    finally:
        store.close()
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        if argv and argv[0] == "owkterm":
            launcher = str(Path(__file__).resolve().parents[1] / "liljack")
            os.execv(launcher, [launcher, *argv])
        if argv and argv[0] == "agent":
            from .agent_launch import main as agent_main
            return agent_main(argv[1:])
        if argv and argv[0] == "room":
            return room_cli(argv[1:])
        if argv and argv[0] == "session":
            return control_cli(argv[1:])
        p = argparse.ArgumentParser(description="lilJack room/session client; launch ./liljack for the native UI")
        p.add_argument("command", choices=["room", "session", "owkterm", "agent"])
        p.parse_args(argv)
        return 2
    except (ValueError, PermissionError, RuntimeError, OSError) as exc:
        print(json.dumps({"error": _scrub(str(exc))}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
