"""Narrow room authority over registry storage; no provider impersonation.

Uses the registry's lock/serialization so phase work does not edit the shared
registry implementation while the heartbeat owner is changing its consumers.
"""
import hashlib
import json
import sqlite3
from contextlib import closing
from pathlib import Path

import liljack_tasks as tasks
from liljack_workspace import clean, clean_truncate, project_key


def read_registry(path):
    registry = tasks.read(path)
    if registry.get('errors'):
        raise ValueError('task registry has parse errors; repair before changing room work')
    return registry


def operator_requested_new_work(root, project, room, since):
    """the operator "requests new work during REVIEW" by SPEAKING in the room after the
    REVIEW transition; that message is the request (the operator, 2026-09-12: "heartbeat
    spanks until the room is done — that is lilJack's rule"; he does not run CLI
    phase commands). The lead may then plan and reopen WORK on his behalf."""
    with closing(sqlite3.connect((Path(root) / 'workspace.sqlite3').as_uri() + '?mode=ro', uri=True)) as db:
        row = db.execute("SELECT 1 FROM messages WHERE project=? AND destination=? AND sender='operator' AND created_at>? LIMIT 1",
                         (project_key(project), room, since or '')).fetchone()
        return row is not None


def _scope(project, room, actor, root):
    with closing(sqlite3.connect((Path(root) / 'workspace.sqlite3').as_uri() + '?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        current = db.execute('SELECT * FROM rooms WHERE id=? AND project=?', (room, project_key(project))).fetchone()
        if not current:
            raise ValueError('room does not belong to project')
        roster = {r['id']: {**json.loads(r['data']), 'role': r['role']} for r in db.execute(
            'SELECT s.id,s.data,t.role FROM room_members m JOIN sessions s ON s.id=m.session_id '
            'LEFT JOIN team t ON t.session_id=s.id WHERE m.room_id=? AND s.project=?', (room, project_key(project)))}
        if actor != 'operator' and actor not in roster:
            raise PermissionError('actor is not a room member')
        leads = [sid for sid, s in roster.items() if s['role'] == 'lead']
        return dict(current), roster, leads


def question_transition(task_id, question_id, actor, operation, text, *, project, room, workspace_root, path=None, restore_state='active'):
    path = Path(path) if path else tasks.default_path()
    with tasks.board.lock(path):
        current, roster, leads = _scope(project, room, actor, workspace_root)
        with closing(sqlite3.connect((Path(workspace_root) / 'workspace.sqlite3').as_uri() + '?mode=ro', uri=True)) as db:
            db.row_factory = sqlite3.Row
            question = db.execute('SELECT * FROM room_questions WHERE id=? AND project=? AND room=? AND task=?',
                                  (question_id, project_key(project), room, task_id)).fetchone()
        if question is None:
            raise ValueError('durable question intent is required')
        registry = read_registry(path)
        task = tasks._find(registry, task_id)
        if task['room'] != room or task['owner_session'] != question['asker']:
            raise PermissionError('todo ownership changed; reconcile before answering')
        mark = f'Question {question_id}:'
        answered = f'Answered {question_id} by {actor}:'
        if operation == 'block':
            if actor != question['asker'] or question['status'] != 'opening':
                raise PermissionError('only the asker can park its durable question')
            provider = roster[actor]['agent']
            if task['owner'] != ('operator' if provider == 'shell' else provider):
                raise PermissionError('todo provider and asking session disagree')
            if task['state'] == 'blocked' and task['note'].startswith(mark):
                return dict(task)
            if task['state'] not in ('assigned', 'active'):
                raise ValueError('todo is no longer actionable; question remains visible for reconciliation')
            state, note = 'blocked', mark + ' ' + text
        elif operation == 'answer':
            if actor != 'operator' and (len(leads) != 1 or actor != leads[0]):
                raise PermissionError('only the current room lead or the operator answers')
            if question['status'] != 'answering' or question['answered_by'] != actor:
                raise PermissionError('durable answer intent must match the answering actor')
            if task['note'].startswith(answered) or any(
                    r['id'] == task_id and r['op'] == 'question-answer' and r['note'].startswith(answered)
                    for r in registry['log']):
                return dict(task)  # Replay must not reset later progress/completion.
            if task['state'] != 'blocked' or not task['note'].startswith(mark):
                raise ValueError('todo block changed; refusing to unblock unrelated work')
            if restore_state not in ('assigned', 'active'):
                raise ValueError('invalid restored task state')
            state, note = restore_state, answered + ' ' + text
        else:
            raise ValueError('unknown question registry operation')
        task.update(state=state, note=note[:tasks.MAX_NOTE], updated=tasks.now())
        tasks._log(registry, task_id, state, 'question-' + operation, actor, note)
        tasks._save(registry, path)
        return dict(task)


def plan_tasks(project, room, actor, items, request_id, *, workspace_root, path=None):
    if not isinstance(items, list) or not 1 <= len(items) <= 100:
        raise ValueError('plan needs 1..100 todos')
    request_id = clean(request_id, 128)
    normalized = []
    for item in items:
        if not isinstance(item, dict) or set(item) - {'title', 'owner_session', 'acceptance'}:
            raise ValueError('todo fields: title, owner_session, optional acceptance')
        normalized.append({'title': clean_truncate(item.get('title'), tasks.MAX_TITLE),
                           'owner_session': clean(item.get('owner_session'), 80),
                           'acceptance': clean(item['acceptance'], tasks.MAX_FIELD) if item.get('acceptance') else ''})
    path = Path(path) if path else tasks.default_path()
    source = 'room-plan:' + hashlib.sha256(json.dumps([project_key(project), room, request_id]).encode()).hexdigest()[:32]
    with tasks.board.lock(path):
        current, roster, leads = _scope(project, room, actor, workspace_root)
        if actor != 'operator' and (len(leads) != 1 or actor != leads[0]):
            raise PermissionError('only the current room lead or the operator plans todos')
        operator_asked = current['phase'] == 'REVIEW' and (actor == 'operator' or operator_requested_new_work(workspace_root, project, room, current.get('phase_at')))
        if current['phase'] != 'PLAN' and not operator_asked:
            raise ValueError('plan todos during PLAN; the operator may request new work during REVIEW (a message from the operator in the room after the REVIEW transition counts)')
        if current['state'] != 'active':
            raise ValueError('activate the room before planning work')
        for item in normalized:
            if item['owner_session'] not in roster:
                raise PermissionError('todo owner_session must belong to this room')
        registry = read_registry(path)
        existing = [t for t in registry['tasks'] if t['source'] == source]
        if existing:
            if [{k: t[k] for k in normalized[0]} for t in existing] != normalized:
                raise ValueError('request_id already used for a different plan')
            return existing
        taken = {t['id'] for t in registry['tasks']}
        made = []
        for item in normalized:
            base = tasks._norm_id(item['title'])
            identity, n = base, 2
            while identity in taken:
                identity = f'{base}-{n}'
                n += 1
            taken.add(identity)
            provider = roster[item['owner_session']]['agent']
            when = tasks.now()
            row = dict(id=identity, state='assigned', title=item['title'], owner='operator' if provider == 'shell' else provider,
                       owner_session=item['owner_session'], room=room, by=actor, progress='', note='', acceptance=item['acceptance'],
                       avoid='', report=tasks.REPORT_DEFAULT, source=source, mail='', session='', created=when, updated=when)
            registry['tasks'].append(row)
            tasks._log(registry, identity, 'assigned', 'room-plan', actor)
            made.append(dict(row))
        tasks._save(registry, path)
        return made
