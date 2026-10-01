"""Room startup coordination; durable intents precede external side effects.

SQLite, the task registry and tmux are separate stores. A retry reads the
recorded outcome; an interrupted launch is never blindly replayed.
"""
from __future__ import annotations

import fcntl
import hashlib
import inspect
import json
from pathlib import Path
import shlex
import uuid

import liljack_tasks as tasks
from liljack_mail import _scrub
from liljack_workspace import Workspace, clean, project_key, session_key, utcnow

AGENTS = ('claude', 'codex', 'deepseek', 'shell')
ROLES = ('lead', 'worker', 'reviewer')
BACKENDS = ('tmux', 'native')


def normalize(project, req):
    fields = Workspace.room_fields(project, req.get('name'), req.get('folder'),
                                   req.get('purpose', ''), validate_folder=False)
    backend = req.get('backend', 'tmux')
    if backend not in BACKENDS:
        raise ValueError('backend must be tmux or native')
    agents, todo = req.get('agents'), req.get('todo', [])
    if not isinstance(agents, list) or not 1 <= len(agents) <= 16:
        raise ValueError('agents must contain 1..16 members')
    normalized_agents = []
    for agent in agents:
        if (not isinstance(agent, dict) or not {'agent', 'role'} <= set(agent) <= {'agent', 'role', 'session'} or
                agent['agent'] not in AGENTS or agent['role'] not in ROLES):
            raise ValueError('each agent requires an available agent type and lead/worker/reviewer role, '
                             'with an optional existing session id')
        entry = {'agent': agent['agent'], 'role': agent['role']}
        sid = agent.get('session')
        if sid is not None:
            sid = str(sid).strip()
            if not sid or len(sid) > 80:
                raise ValueError('agent session must be an existing session id (1..80 characters)')
            entry['session'] = sid
        normalized_agents.append(entry)
    leads = [i for i, agent in enumerate(agents) if agent['role'] == 'lead']
    if len(leads) != 1:
        raise ValueError('room_start requires exactly one lead')
    if not isinstance(todo, list) or len(todo) > 100:
        raise ValueError('todo must be a list of at most 100 items')
    normalized_todo = []
    for item in todo:
        if not isinstance(item, dict) or set(item) - {'title', 'assignee'}:
            raise ValueError('each TODO requires title and optional assignee index')
        title = clean(item.get('title'), tasks.MAX_TITLE).strip()
        index = item.get('assignee', leads[0])
        if type(index) is not int or not 0 <= index < len(agents):
            raise ValueError('TODO assignee must be a zero-based agents index')
        if any(ord(c) < 32 or 127 <= ord(c) < 160 for c in title):
            raise ValueError('TODO title must be a single line without controls')
        normalized_todo.append({'title': title, 'assignee': index})
    return {**fields, 'agents': normalized_agents, 'todo': normalized_todo, 'backend': backend}


def planned_session(tmux, project, folder, agent, backend='tmux'):
    name = 'lj-' + uuid.uuid4().hex[:16]
    native = f'{tmux.socket}/{name}' if backend == 'tmux' else name
    return {'id': session_key(backend, native), 'session': native, 'harness': backend,
            'backend': backend, 'agent': agent, 'cwd': folder, 'project': project_key(project),
            'tmux_name': name, 'state': 'planned', 'event': 'RoomStart', 'at': utcnow()}


def room_context(store, project, room_id, viewer, *, task_path=None, acknowledge=False):
    from .room_protocol import status as protocol_status
    project = project_key(project)
    store._participant(project, viewer)
    room = store._room(project, room_id)
    members = [store._decorate(json.loads(r[0])) for r in store.db.execute(
        'SELECT s.data FROM sessions s JOIN room_members m ON m.session_id=s.id '
        'WHERE m.room_id=? AND s.project=? ORDER BY s.rowid', (room_id, project))]
    mine = next((m for m in members if m['id'] == viewer), None)
    if viewer != 'operator' and not mine:
        raise PermissionError('join this room before reading its context')
    own_tasks = (tasks.tasks(room=room_id, open_only=True, path=task_path) if viewer == 'operator' else
                 tasks.room_tasks(project, room_id, viewer, path=task_path, workspace_root=store.root))
    # ⚠ THE ACTIONABLE TAB MUST NOT BE EMPTY WHILE THE ROOM WORKS. The todo view
    # was room-scoped only, so a handoff room whose work lived on tasks owned by
    # its members (not filed to the room) showed "no todos" all night while 15
    # registry tasks carried the work. Merge the members' registry tasks, each
    # marked source:"registry" so the UI can tell a queue todo from a board row.
    own_tasks = _with_member_registry_tasks(own_tasks, members, task_path, room_id,
                                            ambiguous=_agents_elsewhere(store, project, room_id))
    others = []
    for r in store.db.execute("SELECT id,name,purpose,state FROM rooms WHERE project=? AND id!=? AND state!='removed' ORDER BY rowid",
                              (project, room_id)):
        try:
            count = len(tasks.tasks(room=r['id'], open_only=True, path=task_path))
        except (OSError, ValueError):
            count = None
        others.append({'name': r['name'], 'purpose': r['purpose'], 'state': r['state'], 'open_todo_count': count})
    if acknowledge and mine:
        store.intro_state(viewer, 'acknowledged')
    # ⚠ READ THE STATUS AFTER ACKNOWLEDGING. Capturing it first returned the
    # pre-acknowledge intro_state and made a just-acknowledged room read as
    # 'submitted' to the very context call that acknowledged it.
    status = store.room_start_status(room_id)
    moved = any(s.get('moved') and s.get('session_id') == viewer
                for s in status.get('started_sessions', []))
    # ⚠ THE TWO CURSORS THE STANDING SET DEMANDS. "Distinguish the message
    # sequence (chronological) from the event cursor (snapshot)" is unanswerable
    # when --context returns neither (item 8: every agent opened with a paragraph
    # about which cursor it did NOT get). `last_seq` is the messages table's
    # monotonic seq (what an agent "last read"); `cursor` is the events table's
    # max seq, the snapshot point to pass back to `--since` for the delta.
    last_seq = store.db.execute("SELECT COALESCE(MAX(seq),0) FROM messages").fetchone()[0]
    cursor = store.db.execute("SELECT COALESCE(MAX(seq),0) FROM events").fetchone()[0]
    return {'room': room, 'role': mine['role'] if mine else 'operator',
            'moved': moved,
            'last_seq': last_seq, 'cursor': cursor,
            'members': [{'session_id': m['id'], 'agent': m['agent'], 'role': m['role'],
                         'state': m['state']} for m in members],
            'todo': own_tasks, 'other_rooms': others, **status,
            'protocol': protocol_status(store, project, room_id)}


def _agents_elsewhere(store, project, room_id):
    """Agent names holding a session in some OTHER live room of this project.

    ⚠ MULTI-ROOM LEAK (the operator 2026-09-20, r-bcbc0454). The fallback below matched
    an unfiled row (room="") by AGENT NAME, so every unfiled claude row filled
    every room where any claude session sat, while worker_wake.room_allows()
    never drove a room-less row to a session in a room: shown everywhere, nudged
    nowhere. An agent present in two live rooms cannot answer "whose room is
    this?" by name — such rows are shown in neither until filed or bound.
    """
    out = set()
    try:
        rows = store.db.execute(
            "SELECT s.data FROM room_members m JOIN rooms r ON r.id=m.room_id "
            "LEFT JOIN sessions s ON s.id=m.session_id "
            "WHERE r.project=? AND r.id!=? AND r.state NOT IN ('removed','archived','resolved')",
            (project, room_id)).fetchall()
    except Exception:
        return out
    for (blob,) in rows:
        try:
            agent = (json.loads(blob) if blob else {}).get('agent') or ''
        except Exception:
            agent = ''
        if agent:
            out.add(str(agent).strip().lower())
    return out


def _with_member_registry_tasks(base, members, task_path, room_id, ambiguous=frozenset()):
    """Registry tasks owned by room members — a FALLBACK when the scoped view is
    empty, so the ACTIONABLE tab is never empty while the room works.

    ⚠ THE SCOPED VIEW WINS WHEN IT HAS ANYTHING. Widening every viewer to every
    member's queue broke the registry's tested own-only isolation (a worker read
    the reviewer's private task, and another room's task leaked into an intro).
    The empty-tab defect is exactly "no room-scoped todo, but the room's members
    hold work", so the merge fires only there. Every entry is marked
    source:"registry" so the UI can tell a queue todo from a board row.

    ⚠ A TASK FILED TO ANOTHER ROOM IS NOT THIS ROOM'S WORK. Matching on
    owner_session/owner alone leaked an ARCHIVED room's 7 tasks into whatever room
    the same agents later sat in (the operator live 01:10: "context is leaking between
    rooms"). A task whose `room` is set and != this room is skipped; an UNFILED
    task (empty room) stays eligible — that is exactly the handoff case the
    fallback exists for.
    """
    marked = [{**t, 'source': 'registry'} for t in base]
    if marked:
        return marked
    member_ids = {m['id'] for m in members}
    member_agents = {str(m.get('agent') or '').strip().lower() for m in members}
    try:
        rows = tasks.tasks(open_only=True, path=task_path)
    except (OSError, ValueError):
        return marked
    for t in rows:
        filed = str(t.get('room') or '').strip()
        if filed and filed != room_id:
            continue
        bound = str(t.get('owner_session') or '').strip()
        agent = str(t.get('owner') or '').strip().lower()
        # ⚠ NAME MATCH ONLY WHEN THE NAME IS UNAMBIGUOUS (see _agents_elsewhere).
        owned = (bound in member_ids if bound
                 else agent in member_agents and agent not in ambiguous)
        if not owned:
            continue
        marked.append({**t, 'source': 'registry'})
    return marked


def _write_room_instructions(store, project, room_id, folder):
    """T5: a marked CLAUDE.md/AGENTS.md fragment in the room folder so the repo
    rooms are not blind after compaction. Best effort: never fails the launch."""
    try:
        from .room_instructions import ensure_fragment, overview_text
        room = store._room(project_key(project), room_id)
        brain = Path(__file__).resolve().parents[2]
        ensure_fragment(folder, room_id, room['name'], room.get('purpose', ''), project_key(project),
                        _onboarding_lines(), overview_text(brain))
    except Exception:
        pass


def _onboarding_lines():
    """Canonical standing instruction set for every agent, on room join or room
    creation. Its job is to STOP parallel functionality: a second board, a second
    registry, a second message channel. The coordination surface already exists
    and is named here, once, so nobody rebuilds it out of confusion."""
    return [
        'STANDING INSTRUCTION SET (lilJack coordination — already exists, do not rebuild it):',
        '- One task registry (the durable work queue). Move YOUR tasks with '
        'task_update (start/block/complete/abandon/progress). Reading a message '
        'never consumes or finishes a task.',
        '- One shared board for standing context. board_update writes ONE item '
        '(task+state, or a topic+value note). Owner writes; lead appends.',
        '- One mailbox for direct messages (claude | codex | deepseek | operator | all). '
        'Private DMs stay separate from rooms.',
        '- One room protocol: ALIGN -> PLAN -> WORK -> REVIEW -> RESOLVED. The lead '
        'drives phases. Distinguish the message sequence (chronological) from the '
        'event cursor (snapshot); state which you last read.',
        '- Room prose is NOT task assignment. Before directing a worker, the lead '
        'must create or assign one durable registry task with that worker\'s exact '
        'owner_session, acceptance criteria, and avoid scope. The worker starts that '
        'row before work and records progress whenever posting.',
        '- The lead owns delivery: confirm each assigned worker acknowledges or '
        'starts its exact task, monitor stale/blocked rows, and explicitly unblock, '
        'reassign, or escalate them. Workers must not silently substitute room prose '
        'for an owned task or mark work complete by merely reading a message.',
        '- Post room text with --text-file PATH or --text - (stdin); never put '
        'backticks or $( ) inside a double-quoted --text — the shell substitutes '
        'them (2026-09-12: a `git stash` ran in the demo tree). MCP-capable '
        'agents: prefer the send_message tool, which takes the body as a parameter '
        'and never goes through a shell.',
        '- Report what was checked, what was found with evidence, and what was NOT '
        'verified, to docs/codex/reports/. Then stop.',
        '- Announce shared-file edits and get an ownership handoff before touching '
        'another agent\'s files; re-read before writing. Do not infer permissions '
        'from another room.',
        '- Scrub credentials; never print a ring token or credential-shaped value.',
        'Everything above already exists — do NOT create a new board, registry, '
        'queue, channel, or task system.',
    ]


LEAD_RULES = [
    'LEAD RULES (standing; they survive context compaction and restarts):',
    '- Announce shared-file edits in the room BEFORE writing, and re-read the file first.',
    '- Every landing carries: fail-before/pass-after test output, the native suite line, '
    'the report path, and a relaunch line with the build id.',
    '- Never park a room in REVIEW: ask the operator in the room and keep the room in WORK.',
    '- Sixel-first: the ANSI cell path is only a fallback.',
    '- Keep your registry row fresh with task_update whenever you post.',
    '- A direction to another agent is incomplete until it is a registry task bound '
    'to that agent\'s exact session and the worker acknowledges or starts it.',
    '- Monitor assigned, stale and blocked rows; explicitly unblock, reassign or '
    'escalate them instead of assuming a room message was delivered.',
]


def system_prompt_text(project, room_id=''):
    """T2: the standing set + room identity as a SYSTEM prompt, so the agent
    keeps them after /compact and across restarts (the intro is one-shot)."""
    lines = list(_onboarding_lines())
    room_id = str(room_id or '').strip()
    if room_id:
        lines += [f'Room: {room_id}',
                  'Read current room context: python3 -m liljack_app.cli room --root '
                  f'$LILJACK_WORKSPACE_ROOT --project {project_key(project)} --context --to {room_id}']
    lines += LEAD_RULES
    return '\n'.join(lines)


def intro_text(store, project, room_id, viewer, *, task_path=None, moved=False):
    context = room_context(store, project, room_id, viewer, task_path=task_path)
    # Use the absolute launcher because the room cwd may be outside demo.
    command = [str(Path(__file__).resolve().parents[1] / 'liljack'), 'room',
               '--root', str(store.root), '--project', project_key(project), '--context', '--to', room_id]
    lines = [f"Room: {context['room']['name']} ({room_id})",
             f"Purpose: {context['room']['purpose']}", f"Folder: {context['room']['folder']}",
             f"Your session: {viewer}; role: {context['role']}.",
             f"Context snapshot: message sequence {context['last_seq']}, event cursor {context['cursor']}. "
             f"State which you last read; pass the event cursor to `--since` for the delta."]
    if moved:
        lines.append('You were MOVED into this room as an existing session — not spawned. '
                     'Your terminal, history and identity are unchanged; only your room and role are new.')
    lines.append('Team (launch states are a snapshot; some teammates may still be starting):')
    # ⚠ A CLOSED SESSION IS NOT A TEAMMATE YOU CAN TALK TO. The roster listed
    # EVERY member the room has ever had, each on its own line with a 40-char
    # session id. Measured 2026-09-10: 16 lines of which 10 were exited or
    # stopped — noise charged to the opening turn of every agent spawned, and
    # four consecutive codex sessions each spent that turn on onboarding without
    # reaching the work. Live members are named in full because you address
    # them; closed ones are counted, because the only fact that survives them is
    # that they existed.
    _CLOSED = ('exited', 'stopped', 'ended')
    _live = [m for m in context['members'] if str(m.get('state', '')).lower() not in _CLOSED]
    _gone = [m for m in context['members'] if str(m.get('state', '')).lower() in _CLOSED]
    lines += [f"- {m['session_id']}: {m['agent']} / {m['role']} / {m['state']}" for m in _live]
    if _gone:
        counts = {}
        for m in _gone:
            counts[m['agent']] = counts.get(m['agent'], 0) + 1
        summary = ', '.join(f"{a} x{n}" if n > 1 else a for a, n in sorted(counts.items()))
        lines += [f"- (+{len(_gone)} closed session(s) not listed: {summary})"]
    if not _live:
        lines += ['- (no live teammates yet)']
    lines += ['Your TODO view (lead sees the room; workers/reviewers see their own):']
    lines += [f"- {t['id']}: {t['title']} [{t['state']}]" for t in context['todo']] or ['- No open TODOs.']
    lines += ['Other rooms (digest only):']
    lines += [f"- {r['name']}: {r['purpose']} | open TODOs: " +
              ('unknown' if r['open_todo_count'] is None else str(r['open_todo_count'])) for r in context['other_rooms']]
    lines += ['Read current context and acknowledge this intro: ' + shlex.join(command),
              'Coordinate and report in this room using its exact room ID. Keep private messages separate.',
              'Follow the working folder instructions. Do not infer permissions from another room.']
    lines += _onboarding_lines()
    if context['room']['phase'] == 'ALIGN':
        align_command = command[:-3] + ['--align', 'Your one-line understanding', '--to', room_id]
        lines += ['ALIGN FIRST. Read the intro, acknowledge with one line of your understanding, then wait.',
                  'Do not start the TODOs until the lead has moved the room through PLAN into WORK.',
                  'Acknowledge alignment: ' + shlex.join(align_command)]
    return clean('\n'.join(lines), 48000)


def _launch(store, tmux, project, row, intro, room_id=''):
    backend = row.get('backend', 'tmux')
    extra = {}
    if room_id and 'room_id' in inspect.signature(tmux.create).parameters:
        # T2: room id for the claude system prompt; older/fake adapters lack it.
        extra['room_id'] = room_id
    sid = tmux.create(row['agent'], project, store.root, cwd=row['cwd'], intro=intro,
                      name=row['tmux_name'], backend=backend, **extra)
    if sid != row['id']:
        raise RuntimeError('Launch returned a different session identity; inspect before retrying')
    live = next((r for r in tmux.sessions(project) if r['id'] == sid), None)
    if not live:
        raise RuntimeError('Launch not discoverable; outcome unknown, inspect before retrying')
    store.observe(live)
    if backend == 'native':
        from .runtime import NativeBackend
        store.bind_terminal(sid, NativeBackend().sock, live['tmux_name'],
                            live.get('daemon_id') or live['tmux_name'], actor='operator')
    else:
        store.bind_terminal(sid, tmux.socket, live['tmux_name'], live['pane'], actor='operator')
    if live.get('state') == 'exited':
        raise RuntimeError('Agent exited during startup; inspect its retained terminal')
    store.intro_state(sid, 'available' if row['agent'] == 'shell' else 'submitted')


def start_room(store, tmux, project, req, *, actor, task_path=None, handoff_root=None):
    if actor != 'operator':
        raise PermissionError('only the operator can start rooms')
    project = project_key(project)
    payload = normalize(project, req)
    request_id = req.get('request_id')
    if not isinstance(request_id, str) or not 1 <= len(request_id) <= 128:
        raise ValueError('request_id must be 1..128 characters')
    digest = hashlib.sha256(json.dumps([project, request_id]).encode()).hexdigest()
    with (store.root / ('room-start-' + digest + '.lock')).open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            existing = store.start_result(project, request_id, payload)
            if existing:
                return existing
            raise RuntimeError('Room startup is in progress; retry this request_id')
        existing = store.start_result(project, request_id, payload)
        if existing:
            if existing['start_status'] == 'starting':
                # Owning the lock proves the previous coordinator has exited.
                existing['start_status'] = 'partial'
                existing['errors'].append({'stage': 'recovery', 'message':
                    'Startup interrupted; reconcile reserved sessions and task_ids before adding agents. No launch replayed.'})
                for member in existing['started_sessions']:
                    if member['state'] == 'launching':
                        member['state'] = 'uncertain'
                        store.intro_state(member['session_id'], 'uncertain')
                    elif member['state'] == 'planned':
                        member['state'] = 'not_started'
                store.save_start(project, request_id, existing)
            return existing
        # ⚠ A MOVE-IN IS NOT A SPAWN. An agent entry may carry an existing
        # "session": that session is MOVED into the new room with the given role
        # and is never launched. A listed session that is not live is an ERROR —
        # silently spawning a fresh one would split the team the handoff exists
        # to carry over, and the empty ACTIONABLE tab this task fixes was exactly
        # the "room created, work left behind" shape.
        try:
            live = {r['id']: r for r in tmux.sessions(project) if r.get('state') != 'exited'}
        except Exception:
            live = {}
        rows = []
        for a in payload['agents']:
            sid = a.get('session')
            if sid:
                found = store.db.execute('SELECT data FROM sessions WHERE id=? AND project=?',
                                         (sid, project)).fetchone()
                if not found:
                    raise ValueError(f'session {sid} is not a session of this project')
                if sid not in live:
                    raise ValueError(f'session {sid} is not live; refusing to move it in (no spawn fallback)')
                data = json.loads(found['data'])
                old = store.db.execute('SELECT room_id FROM room_members WHERE session_id=?',
                                       (sid,)).fetchone()
                digest = None
                try:
                    import liljack_handoff
                    digest = liljack_handoff.load_digest(sid, archive_root=handoff_root)
                except Exception:
                    digest = None
                rows.append({'id': sid, 'move': True, 'agent': a['agent'],
                             'harness': data.get('harness'), 'session': data.get('native_id'),
                             'digest': digest, 'old_room': (old['room_id'] if old else '')})
            else:
                tmux.preflight(a['agent'], payload['folder'], backend=payload['backend'])
                rows.append(planned_session(tmux, project, payload['folder'], a['agent'], payload['backend']))
        # Check registry readability and assignment authority before creating a room.
        tasks.read(task_path)
        for item in payload['todo']:
            provider = payload['agents'][item['assignee']]['agent']
            tasks.authorise(actor, 'propose', owner='operator' if provider == 'shell' else provider)
        result, created = store.reserve_room_start(project, request_id, payload, rows, actor=actor)
        if not created:
            return result
        _write_room_instructions(store, project, result['created_room'], payload['folder'])
        stage, index = 'tasks', None
        try:
            for index, item in enumerate(payload['todo']):
                owner = rows[item['assignee']]
                seeded = tasks.seed_room_tasks(project, result['created_room'],
                    [{'title': item['title'], 'owner': 'operator' if owner['agent'] == 'shell' else owner['agent'],
                      'owner_session': owner['id']}], actor, f'{request_id}:todo:{index}', path=task_path)
                result['task_ids'].extend(t['id'] for t in seeded)
                store.save_start(project, request_id, result)
            stage, index = 'intro', None
            # Prepare all intros before launching anything, so context failure cannot strand half a team.
            # ⚠ A MOVED SESSION RESUMES ON ITS DIGEST, NOT ON STALE NOTICES.
            # Where a handoff digest exists it rides the intro, so the first
            # thing the session reads is what it holds, not the backlog.
            intros = []
            for r in rows:
                base = intro_text(store, project, result['created_room'], r['id'],
                                  task_path=task_path, moved=bool(r.get('move')))
                if r.get('digest'):
                    import liljack_handoff
                    base = base + '\n\n' + liljack_handoff.render_digest(
                        r['digest'], room_id=result['created_room'])
                intros.append(base)
            for row, intro in zip(rows, intros):
                store.save_intro(row['id'], intro)
            # Moved sessions are ALREADY RUNNING: mark them, do not launch.
            for i, row in enumerate(rows):
                if not row.get('move'):
                    continue
                member = result['started_sessions'][i]
                member.update(state='running', moved=True,
                              intro_state='available' if member['agent'] == 'shell' else 'submitted')
                store.intro_state(row['id'], member['intro_state'])
                store.save_start(project, request_id, result)
                # K4: the moved session's first post in the new room is generated
                # from its digest (what it holds), for the lead to check against
                # the registry.
                if row.get('digest'):
                    import liljack_handoff
                    body = liljack_handoff.render_digest(
                        row['digest'], room_id=result['created_room'])[:2000]
                    store.post(project, row['id'], 'HANDOFF (moved in):\n' + body,
                               f'{request_id}:handoff:{i}', destination=result['created_room'])
                # K2: the old room's pending notices for this session are SKIPPED,
                # never deleted — the backlog must not follow the move.
                try:
                    from .room_delivery import skip_pending_for
                    skip_pending_for(store, row['id'], row.get('old_room') or '')
                except Exception:
                    pass
            order = sorted((i for i, row in enumerate(rows) if not row.get('move')),
                           key=lambda i: payload['agents'][i]['role'] != 'lead')
            for index in order:
                stage = 'launch'
                member = result['started_sessions'][index]
                member['state'] = 'launching'
                store.save_start(project, request_id, result)
                _launch(store, tmux, project, rows[index], intros[index],
                        room_id=result.get('created_room') or '')
                member.update(state='running', intro_state='available' if member['agent'] == 'shell' else 'submitted')
                store.save_start(project, request_id, result)
            result['start_status'] = 'complete'
        except Exception as exc:
            if stage == 'launch':
                result['started_sessions'][index].update(state='uncertain', intro_state='uncertain')
                store.intro_state(rows[index]['id'], 'uncertain')
            result['start_status'] = ('partial' if stage == 'launch' or result['task_ids'] else 'failed')
            result['errors'].append({'stage': stage, 'index': index, 'message': _scrub(str(exc))[:500]})
            for member in result['started_sessions']:
                if member['state'] == 'planned':
                    member['state'] = 'not_started'
                if member['state'] in ('not_started', 'uncertain') and not rows[member['index']].get('move'):
                    store.observe({**rows[member['index']], 'state': member['state']})
        store.save_start(project, request_id, result)
        return store.start_result(project, request_id)


def create_session(store, tmux, project, agent, *, actor, room_id='', role='worker',
                   task_path=None, backend='tmux'):
    """Single-session compatibility action with explicit, pre-launch team role."""
    project = project_key(project)
    store.spawn_parent(project, room_id, role, actor=actor)
    if agent not in AGENTS:
        raise ValueError('unknown agent')
    if backend not in BACKENDS:
        raise ValueError('backend must be tmux or native')
    folder = store.room_folder(project, room_id)
    tmux.preflight(agent, folder, backend=backend)
    _write_room_instructions(store, project, room_id, folder)
    row = planned_session(tmux, project, folder, agent, backend)
    store.observe(row)
    store.attach_spawned(project, row['id'], room_id, role, actor=actor)
    try:
        intro = intro_text(store, project, room_id, row['id'], task_path=task_path) if room_id else None
        if intro:
            store.save_intro(row['id'], intro)
        _launch(store, tmux, project, row, intro, room_id=room_id or '')
    except Exception as exc:
        store.intro_state(row['id'], 'uncertain')
        store.observe({**row, 'state': 'uncertain'})
        raise RuntimeError(f"Session {row['id']} reserved; launch incomplete, inspect before retrying: {_scrub(str(exc))}") from exc
    return row['id']
