"""Room work protocol. Durable intents bridge SQLite and the BMD task registry.

No terminal input, service control or inferred availability. The heartbeat owns
transport and retries; this module exposes explicit gates and question strikes.
"""
from __future__ import annotations
import hashlib
import json
import os
import sqlite3
from contextlib import contextmanager, closing
from pathlib import Path

import liljack_tasks as tasks
from liljack_workspace import Workspace, clean, project_key, utcnow

PHASES = ('ALIGN', 'PLAN', 'WORK', 'REVIEW', 'RESOLVED')


def ensure_schema(store):
    store.db.executescript('''
      CREATE TABLE IF NOT EXISTS room_alignment (
        room TEXT NOT NULL, session TEXT NOT NULL, understanding TEXT NOT NULL,
        at TEXT NOT NULL, PRIMARY KEY(room,session));
      CREATE TABLE IF NOT EXISTS room_questions (
        id TEXT PRIMARY KEY, project TEXT NOT NULL, room TEXT NOT NULL,
        task TEXT NOT NULL, asker TEXT NOT NULL, question TEXT NOT NULL,
        status TEXT NOT NULL, previous_state TEXT NOT NULL, request_id TEXT NOT NULL,
        answer TEXT NOT NULL DEFAULT '', answered_by TEXT NOT NULL DEFAULT '',
        created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
        strikes INTEGER NOT NULL DEFAULT 0, escalated_at TEXT,
        reason TEXT NOT NULL DEFAULT '', UNIQUE(project,asker,request_id));
      CREATE TABLE IF NOT EXISTS question_attempts (
        question TEXT NOT NULL, attempt TEXT NOT NULL, reason TEXT NOT NULL,
        PRIMARY KEY(question,attempt));
      CREATE TABLE IF NOT EXISTS protocol_notices (
        id TEXT PRIMARY KEY, project TEXT NOT NULL, sender TEXT NOT NULL,
        destination TEXT NOT NULL, text TEXT NOT NULL, sent INTEGER NOT NULL DEFAULT 0,
        error TEXT NOT NULL DEFAULT '');
    ''')


@contextmanager
def transaction(store):
    store.db.execute('BEGIN IMMEDIATE')
    try:
        yield
        store.db.execute('COMMIT')
    except BaseException:
        store.db.execute('ROLLBACK')
        raise


def _line(value, limit=500):
    value = clean(value, limit).strip()
    if any(ord(c) < 32 or 127 <= ord(c) < 160 for c in value):
        raise ValueError('understanding must be one line without controls')
    return value


def _key(*parts):
    return hashlib.sha256(json.dumps(parts).encode()).hexdigest()[:32]


def _member(store, project, room, actor):
    store._participant(project, actor)
    row = next((s for s in store.room_roster(project, room) if s['id'] == actor), None)
    if actor != 'operator' and row is None:
        raise PermissionError('session must belong to this room')
    return row


def _lead(store, project, room, actor=None):
    leads = [s['id'] for s in store.room_roster(project, room) if s['role'] == 'lead']
    if len(leads) != 1:
        if actor == 'operator':
            return None
        raise ValueError('room needs exactly one lead')
    if actor is not None and actor not in ('operator', leads[0]):
        raise PermissionError('only the current room lead or the operator may do this')
    return leads[0]


def _notice(store, identity, project, sender, destination, text):
    store.db.execute('INSERT OR IGNORE INTO protocol_notices(id,project,sender,destination,text) VALUES(?,?,?,?,?)',
                     (identity, project, sender, destination, clean(text)))


def flush_notices(store, project):
    """Retryable local publication; sending PTY input is the heartbeat's job."""
    result = []
    for row in store.db.execute('SELECT * FROM protocol_notices WHERE project=? AND sent=0 ORDER BY rowid',
                                (project_key(project),)).fetchall():
        try:
            msg = store.post(row['project'], row['sender'], row['text'], 'protocol-' + row['id'], destination=row['destination'])
            store.db.execute("UPDATE protocol_notices SET sent=1,error='' WHERE id=?", (row['id'],))
            result.append({'id': row['id'], 'message': msg['id']})
        except Exception as exc:
            from liljack_mail import _scrub
            reason = _scrub(str(exc))[:500]
            store.db.execute('UPDATE protocol_notices SET error=? WHERE id=?', (reason, row['id']))
            result.append({'id': row['id'], 'error': reason})
    return result


def align(store, project, room, actor, understanding):
    project = project_key(project)
    understanding = _line(understanding)
    with transaction(store):
        current = store._room(project, room)
        member = _member(store, project, room, actor)
        if member is None or member['agent'] == 'shell':
            raise PermissionError('an agent acknowledges its own understanding')
        if current['phase'] != 'ALIGN':
            raise ValueError('room is not aligning')
        old = store.db.execute('SELECT understanding FROM room_alignment WHERE room=? AND session=?', (room, actor)).fetchone()
        if old and old[0] != understanding:
            raise ValueError('understanding already acknowledged; discuss corrections with the lead')
        if not old:
            store.db.execute('INSERT INTO room_alignment VALUES(?,?,?,?)', (room, actor, understanding, utcnow()))
            store.intro_state(actor, 'acknowledged')
            store._emit(project, 'room_aligned', {'room': room, 'session': actor, 'understanding': understanding})
            _notice(store, _key(room, actor, 'align'), project, actor, room,
                    'ALIGN acknowledged: ' + understanding + '\nWaiting for the lead to plan and release WORK.')
    flush_notices(store, project)
    return status(store, project, room)


def status(store, project, room):
    project = project_key(project)
    current = store._room(project, room)
    aligned = [dict(r) for r in store.db.execute('SELECT * FROM room_alignment WHERE room=? ORDER BY at', (room,))]
    done = {r['session'] for r in aligned}
    missing = [s['id'] for s in store.room_roster(project, room) if s['agent'] != 'shell' and s['id'] not in done]
    return {'phase': current['phase'], 'phase_at': current['phase_at'], 'alignment': aligned,
            'awaiting_alignment': missing if current['phase'] == 'ALIGN' else [],
            'questions': question_digest(store, project, room),
            'notice_errors': [dict(r) for r in store.db.execute('SELECT id,error FROM protocol_notices WHERE project=? AND sent=0', (project,))]}


def _todo_rows(rows, questions):
    questions = {q['task']: q for q in questions}
    result = []
    for row in rows:
        q = questions.get(row['id'])
        result.append({key: row.get(key, '') for key in
                       ('id', 'title', 'owner', 'owner_session', 'state', 'progress', 'room', 'blocked_on')})
        result[-1].update(question_id=q['id'] if q else '',
                          blocked_reason=q['question'] if q else
                          (row.get('note', '') if row['state'] == 'blocked' else ''))
    result.sort(key=lambda t: (t['state'] not in tasks.OPEN, not bool(t['question_id'])))
    return result


def open_task_snapshot(store, project, *, task_path=None):
    """Operator fallback: all open registry work, including tasks without a room."""
    try:
        from .room_registry import read_registry
        registry = read_registry(task_path)
        if not registry['exists']:
            raise FileNotFoundError('task registry not found')
        rows = [t for t in registry['tasks'] if t['state'] in tasks.OPEN]
        return {'tasks': _todo_rows(rows, question_digest(store, project)), 'tasks_error': ''}
    except Exception as exc:
        from liljack_mail import _scrub
        return {'tasks': [], 'tasks_error': _scrub(f'{type(exc).__name__}: {exc}')[:500]}


def todo_snapshot(store, project, room, *, task_path=None):
    """Operator projection through the registry's scoped adapter, never guesses empty."""
    try:
        rows = tasks.room_tasks(project, room, 'operator', path=task_path,
                                workspace_root=store.root, include_closed=True, strict=True)
        result = _todo_rows(rows, question_digest(store, project, room))
        return {'todos': result, 'todos_error': ''}
    except Exception as exc:
        from liljack_mail import _scrub
        return {'todos': [], 'todos_error': _scrub(f'{type(exc).__name__}: {exc}')[:500]}


def transition(store, project, room, phase, actor, *, task_path=None):
    project = project_key(project)
    if phase not in PHASES:
        raise ValueError('invalid room phase')
    path = Path(task_path) if task_path else tasks.default_path()
    # Same lock order as registry-scoped actions: registry, then workspace.
    with tasks.board.lock(path), transaction(store):
        current = store._room(project, room)
        _member(store, project, room, actor)
        _lead(store, project, room, actor)
        previous = current['phase']
        if previous == phase:
            return status(store, project, room)
        edges = {'ALIGN': ('PLAN',), 'PLAN': ('WORK',), 'WORK': ('REVIEW',),
                 'REVIEW': ('WORK', 'RESOLVED'), 'RESOLVED': ()}
        if phase not in edges.get(previous, ()):
            raise ValueError(f'illegal phase transition {previous} -> {phase}')
        if current['state'] in ('removed', 'archived'):
            raise ValueError('restore the room before progressing its work')
        if previous == 'REVIEW' and actor != 'operator':
            operator_asked = store.db.execute(
                "SELECT 1 FROM messages WHERE project=? AND destination=? AND sender='operator' AND created_at>? LIMIT 1",
                (project, room, current['phase_at'] or '')).fetchone() is not None
            if not operator_asked:
                raise PermissionError('only the operator may leave REVIEW (a message from the operator in the room after the REVIEW transition counts as his request for new work)')
        from .room_registry import read_registry
        rows = [t for t in read_registry(path)['tasks'] if t['room'] == room]
        pending = [t for t in rows if t['state'] in tasks.OPEN]
        if previous == 'ALIGN' and status(store, project, room)['awaiting_alignment']:
            raise ValueError('waiting for alignment: ' + ', '.join(status(store, project, room)['awaiting_alignment']))
        if phase == 'WORK':
            actionable = [t for t in rows if t['state'] in tasks.PENDING]
            if previous == 'REVIEW':
                old = set(json.loads(current['review_tasks']))
                actionable = [t for t in actionable if t['id'] not in old]
            if not actionable:
                raise ValueError('WORK requires a new actionable todo' if previous == 'REVIEW' else 'WORK requires an actionable todo')
        if phase in ('REVIEW', 'RESOLVED') and (pending or question_digest(store, project, room)):
            raise ValueError('finish all open todos and questions before review/resolution: ' + ', '.join(t['id'] for t in pending))
        when = utcnow()
        baseline = json.dumps([t['id'] for t in rows]) if phase == 'REVIEW' else current['review_tasks']
        store.db.execute('UPDATE rooms SET phase=?,phase_at=?,review_tasks=? WHERE id=?', (phase, when, baseline, room))
        store._emit(project, 'room_phase', {'room': room, 'previous_phase': previous, 'phase': phase, 'actor': actor, 'phase_at': when})
        if phase == 'RESOLVED':
            store.db.execute("UPDATE rooms SET state='resolved',state_at=?,resolved_at=? WHERE id=?", (when, when, room))
            store._emit(project, 'room_state', {'id': room, 'state': 'resolved', 'previous_state': current['state'], 'actor': actor})
        _notice(store, _key(room, when, phase), project, actor, room,
                f'Room phase: {previous} -> {phase}.' + (' the operator, please review this room and resolve it or request new work.' if phase == 'REVIEW' else ''))
    flush_notices(store, project)
    return status(store, project, room)


def question_digest(store, project, room=None):
    rows = store.db.execute("SELECT * FROM room_questions WHERE project=? AND status!='answered' " +
                            ('AND room=? ' if room else '') + 'ORDER BY created_at',
                            (project_key(project), room) if room else (project_key(project),)).fetchall()
    result = []
    for row in rows:
        value = dict(row)
        try:
            value['lead'] = _lead(store, project_key(project), row['room'])
        except ValueError as exc:
            value['lead'] = None
            value['routing_error'] = str(exc)
        result.append(value)
    return result


def task_gate(project, task, root=None):
    """Read-only heartbeat gate. Unknown never grants permission to nudge."""
    if task.get('state') == 'blocked':
        return {'allowed': False, 'reason': task.get('note') or 'task is blocked'}
    room = task.get('room')
    if not room:
        return {'allowed': True, 'reason': 'standalone task'}
    path = Path(root or os.environ.get('LILJACK_WORKSPACE_ROOT', str(Path.home() / '.cache/liljack/workspace'))).expanduser().resolve()
    try:
        with closing(sqlite3.connect((path / 'workspace.sqlite3').as_uri() + '?mode=ro', uri=True)) as con:
            row = con.execute('SELECT phase,state FROM rooms WHERE id=? AND project=?', (room, project_key(project))).fetchone()
            if row is None:
                return {'allowed': False, 'reason': 'unknown room; wait', 'unknown': True}
            from liljack_workspace import ROOM_STATES
            if row[0] not in PHASES or row[1] not in ROOM_STATES:
                return {'allowed': False, 'reason': 'unknown room phase/state; wait', 'unknown': True}
            if row != ('WORK', 'active'):
                return {'allowed': False, 'reason': f'room phase {row[0]}, state {row[1]}; wait'}
            question = con.execute("SELECT id FROM room_questions WHERE room=? AND task=? AND status!='answered'", (room, task['id'])).fetchone()
            if question:
                return {'allowed': False, 'reason': f'blocked on question {question[0]}'}
        return {'allowed': True, 'reason': 'WORK phase'}
    except (OSError, sqlite3.Error, ValueError) as exc:
        return {'allowed': False, 'reason': f'cannot tell room phase; wait: {type(exc).__name__}', 'unknown': True}


def ask(store, project, room, task_id, actor, question, request_id, *, task_path=None):
    project = project_key(project)
    question = clean(question, 2000)
    request_id = clean(request_id, 128)
    with transaction(store):
        current = store._room(project, room)
        member = _member(store, project, room, actor)
        if member is None:
            raise PermissionError('a question requires its asking session')
        if current['phase'] != 'WORK':
            raise ValueError('questions about work require WORK phase')
        existing = store.db.execute('SELECT * FROM room_questions WHERE project=? AND asker=? AND request_id=?',
                                    (project, actor, request_id)).fetchone()
        if existing:
            value = dict(existing)
            if (value['room'], value['task'], value['question']) != (room, task_id, question):
                raise ValueError('request_id already used for a different question')
        else:
            task = tasks.get(task_id, path=task_path)
            if task['room'] != room or task['owner_session'] != actor:
                raise PermissionError('asker must own this exact room todo session')
            if task['state'] not in ('assigned', 'active'):
                raise ValueError('only assigned or active work can be parked with a question')
            if store.db.execute("SELECT 1 FROM room_questions WHERE task=? AND project=? AND status!='answered'", (task_id, project)).fetchone():
                raise ValueError('task already has an unanswered question')
            identity = 'q-' + _key(project, actor, request_id)
            when = utcnow()
            store.db.execute('INSERT INTO room_questions(id,project,room,task,asker,question,status,previous_state,request_id,created_at,updated_at) '
                             "VALUES(?,?,?,?,?,?,'opening',?,?,?,?)", (identity, project, room, task_id, actor, question, task['state'], request_id, when, when))
            store._emit(project, 'room_question_intent', {'id': identity, 'room': room, 'task': task_id, 'asker': actor, 'question': question})
            value = dict(store.db.execute('SELECT * FROM room_questions WHERE id=?', (identity,)).fetchone())
    if value['status'] == 'opening':
        _finish_question(store, value, task_path=task_path)
    flush_notices(store, project)
    return dict(store.db.execute('SELECT * FROM room_questions WHERE id=?', (value['id'],)).fetchone())


def _finish_question(store, value, *, task_path=None):
    try:
        from .room_registry import question_transition
        question_transition(value['task'], value['id'], value['asker'],
            'block', value['question'], project=value['project'], room=value['room'],
            workspace_root=store.root, path=task_path)
        with transaction(store):
            store.db.execute("UPDATE room_questions SET status='open',reason='',updated_at=? WHERE id=? AND status='opening'", (utcnow(), value['id']))
            store._emit(value['project'], 'room_question', {'id': value['id'], 'task': value['task'], 'room': value['room']})
            _notice(store, value['id'] + '-ask', value['project'], value['asker'], value['room'],
                    f"Question {value['id']} blocks todo {value['task']}. Current room lead, please answer:\n{value['question']}")
    except Exception as exc:
        from liljack_mail import _scrub
        store.db.execute('UPDATE room_questions SET reason=? WHERE id=?', (_scrub(str(exc))[:500], value['id']))
        raise


def answer(store, project, question_id, actor, text, *, task_path=None):
    project = project_key(project)
    text = clean(text, 2000)
    with transaction(store):
        row = store.db.execute('SELECT * FROM room_questions WHERE id=? AND project=?', (question_id, project)).fetchone()
        if row is None:
            raise ValueError('question does not belong to this project')
        value = dict(row)
        _lead(store, project, value['room'], actor)
        if value['status'] == 'opening':
            raise ValueError('question blocking is incomplete; retry/reconcile it first')
        if value['answer'] and (value['answer'], value['answered_by']) != (text, actor):
            raise ValueError('question already has a different answer')
        if value['status'] == 'answered':
            return value
        if value['status'] != 'answering':
            store.db.execute("UPDATE room_questions SET status='answering',answer=?,answered_by=?,updated_at=? WHERE id=?", (text, actor, utcnow(), question_id))
            store._emit(project, 'room_answer_intent', {'id': question_id, 'actor': actor, 'answer': text})
        value.update(answer=text, answered_by=actor)
    _finish_answer(store, value, task_path=task_path)
    flush_notices(store, project)
    return dict(store.db.execute('SELECT * FROM room_questions WHERE id=?', (question_id,)).fetchone())


def _finish_answer(store, value, *, task_path=None):
    try:
        from .room_registry import question_transition
        question_transition(value['task'], value['id'], value['answered_by'],
            'answer', value['answer'], project=value['project'], room=value['room'],
            workspace_root=store.root, path=task_path, restore_state=value['previous_state'])
        with transaction(store):
            store.db.execute("UPDATE room_questions SET status='answered',reason='',updated_at=? WHERE id=?", (utcnow(), value['id']))
            store._emit(value['project'], 'room_question_answered', {'id': value['id'], 'actor': value['answered_by'], 'answer': value['answer']})
            body = f"Answer to {value['id']} for todo {value['task']}:\n{value['answer']}\nThe todo is unblocked."
            _notice(store, value['id'] + '-answer-room', value['project'], value['answered_by'], value['room'], body)
            _notice(store, value['id'] + '-answer-asker', value['project'], value['answered_by'], value['asker'], body)
    except Exception as exc:
        from liljack_mail import _scrub
        store.db.execute('UPDATE room_questions SET reason=? WHERE id=?', (_scrub(str(exc))[:500], value['id']))
        raise


def reconcile(store, project, *, task_path=None):
    errors = []
    for row in store.db.execute("SELECT * FROM room_questions WHERE project=? AND status IN ('opening','answering') ORDER BY created_at", (project_key(project),)).fetchall():
        try:
            (_finish_question if row['status'] == 'opening' else _finish_answer)(store, dict(row), task_path=task_path)
        except Exception as exc:
            from liljack_mail import _scrub
            errors.append({'question': row['id'], 'reason': _scrub(str(exc))[:500]})
    return {'errors': errors, 'notices': flush_notices(store, project)}


def question_strike(store, project, question_id, attempt_id, reason, immediate=False):
    project = project_key(project)
    attempt_id, reason = clean(attempt_id, 128), clean(reason, 500)
    with transaction(store):
        row = store.db.execute("SELECT * FROM room_questions WHERE id=? AND project=? AND status!='answered'", (question_id, project)).fetchone()
        if row is None:
            raise ValueError('unanswered question not found')
        inserted = store.db.execute('INSERT OR IGNORE INTO question_attempts VALUES(?,?,?)', (question_id, attempt_id, reason)).rowcount
        if inserted:
            strikes = row['strikes'] + 1
            store.db.execute('UPDATE room_questions SET strikes=?,reason=?,updated_at=? WHERE id=?', (strikes, reason, utcnow(), question_id))
            store._emit(project, 'room_question_strike', {'id': question_id, 'attempt': attempt_id, 'strikes': strikes, 'reason': reason})
            if (immediate or strikes >= 3) and not row['escalated_at']:
                store.db.execute('UPDATE room_questions SET escalated_at=? WHERE id=?', (utcnow(), question_id))
                store._emit(project, 'room_question_escalated', {'id': question_id, 'to': 'operator', 'reason': reason})
                _notice(store, question_id + '-escalate', project, row['asker'], 'operator',
                        f"Automated question escalation to the operator: {question_id}, todo {row['task']}. Lead has not answered: {reason}\n{row['question']}")
    flush_notices(store, project)
    return dict(store.db.execute('SELECT * FROM room_questions WHERE id=?', (question_id,)).fetchone())


def plan(store, project, room, actor, items, request_id, *, task_path=None):
    from .room_registry import plan_tasks
    result = plan_tasks(project, room, actor, items, request_id, workspace_root=store.root, path=task_path)
    with transaction(store):
        marker = _key(project_key(project), room, request_id, 'plan')
        if not store.db.execute('SELECT 1 FROM protocol_notices WHERE id=?', (marker,)).fetchone():
            store._emit(project_key(project), 'room_plan', {'room': room, 'actor': actor, 'task_ids': [t['id'] for t in result]})
            _notice(store, marker, project_key(project), actor, room, 'Room plan todos: ' + ', '.join(t['id'] for t in result))
    flush_notices(store, project)
    return {'todo': result, **status(store, project, room)}
